#include "backtester/harness/strategy_harness.h"

#include "backtester/harness/wslog_reader.h"
#include "strategy/strategy/strategy_factory.h"

#include <messages/ExchangeId.h>
#include <messages/ExchangeRegistry.h>

#include <bpt_common/logging.h>
#include <stdexcept>
#include <string_view>

namespace bpt::backtester::harness {

namespace {

bpt::strategy::refdata::InstrumentType map_inst_type(const std::string& t) {
    if (t == "PERP")
        return bpt::strategy::refdata::InstrumentType::PERPETUAL;
    if (t == "FUTURE")
        return bpt::strategy::refdata::InstrumentType::FUTURE;
    if (t == "OPTION")
        return bpt::strategy::refdata::InstrumentType::OPTION;
    return bpt::strategy::refdata::InstrumentType::SPOT;
}

}  // namespace

StrategyHarness::StrategyHarness(Options opts) : opts_(std::move(opts)) {}

StrategyHarness::~StrategyHarness() = default;

void StrategyHarness::initialize() {
    // Order gateway adapter — bridges strategy's IOrderGatewayClient
    // calls into the matching engine.
    order_gw_ = std::make_unique<InProcessOrderGatewayClient>(matching_);

    // In-process refdata + MD clients. Strategy holds these by
    // interface; harness keeps the concrete pointers so it can drive
    // the push_* methods.
    refdata_client_ = std::make_unique<bpt::strategy::refdata::InProcessRefdataClient>();
    md_client_ = std::make_unique<bpt::strategy::md::InProcessMdClient<HarnessHandler>>();

    // Load instrument mapping JSON into the refdata client's cache.
    // bpt-refdata's mapping_lib does the parsing — same loader the
    // production refdata service uses. We then convert each
    // InstrumentEntry to a refdata::Instrument and seed the cache
    // directly (bypassing SBE serialisation that would happen in the
    // live Aeron path).
    bpt::refdata::mapping::InstrumentMappingLoader mapping;
    mapping.load(opts_.instrument_mapping_path);

    std::vector<bpt::strategy::refdata::Instrument> seed;
    for (const auto& venue : bpt::messages::ExchangeRegistry::kEntries) {
        const auto entries = mapping.instruments_for_venue(static_cast<uint8_t>(venue.id));
        for (const auto& e : entries) {
            bpt::strategy::refdata::Instrument inst{};
            inst.instrument_id = e.canonical_id;
            inst.symbol = e.venue_symbol;
            inst.exchange = std::string{venue.name};
            inst.base_currency = e.info.base;
            // HL is USD-quoted natively (synthetic USD perpetuals — there
            // is no USDT pair on HL). The mapping JSON happens to record
            // quote="USDT" for HL because bpt-tape's universe builder
            // normalises that way; the live refdata service's HL adapter
            // emits quote="USD" so strategy configs use BASE/USD:PERPETUAL.
            // Mirror the live normalisation here.
            inst.quote_currency =
                (venue.id == bpt::messages::ExchangeId::HYPERLIQUID) ? std::string{"USD"} : e.info.quote;
            inst.type = map_inst_type(e.info.type);
            // tick_size / lot_size left at 0 — strategy treats 0 as
            // "unknown" and skips tick rounding. For full fidelity
            // these would come from a venue-meta snapshot (HL meta.json
            // etc.); deferred until the harness needs realistic
            // rounding behavior.
            seed.push_back(std::move(inst));
        }
    }
    refdata_client_->mutable_cache().seed(std::move(seed));
    refdata_client_->set_ready_state(
        /*exchanges_loaded=*/0xFF,  // pretend every configured exchange is up
        /*instrument_count=*/static_cast<uint16_t>(refdata_client_->cache().size()),
        /*fee_schedules_loaded=*/true,
        /*funding_rates_loaded=*/true);

    // Strategy config — uses the same loader StrategyService does, so
    // params land identically.
    strategy_cfg_ = bpt::strategy::config::AppConfig::load(opts_.strategy_config_path);

    // OrderManager wraps the order gateway + the refdata cache (used
    // for symbol → instrument resolution on send paths).
    order_mgr_ = std::make_unique<bpt::strategy::order::OrderManager>(*order_gw_, refdata_client_->cache());

    // Strategy.
    strategy_ = bpt::strategy::strategy::StrategyFactory::create(strategy_cfg_.strat,
                                                                 *refdata_client_,
                                                                 md_client_.get(),
                                                                 order_mgr_.get(),
                                                                 /*vol_client=*/nullptr);

    // Wire strategy callbacks. These mirror StrategyService::wire_*_callbacks()
    // but are simpler because we don't need the latency histograms,
    // console publish, or watchdog logic — those are integration
    // concerns that belong on the multi-process path.
    refdata_client_->on_snapshot_complete = [this](const bpt::strategy::refdata::InstrumentCache& cache) {
        strategy_->on_snapshot(cache);
    };
    refdata_client_->on_delta = [this](const bpt::strategy::refdata::Instrument& inst,
                                       bpt::messages::DeltaUpdateType::Value t) {
        strategy_->on_delta(inst, t);
    };
    md_handler_.strategy = strategy_.get();
    md_client_->set_handler(&md_handler_);
    order_gw_->on_exec_report = [this](const bpt::messages::ExecutionReport& rpt) {
        strategy_->on_exec_report(rpt);
    };

    // Results collector. Ownership of fees + run metadata follows the
    // same shape as ClockMaster's wiring in the multi-process path.
    bpt::backtester::results::ResultsCollector::RunMetadata md_meta{};
    // (RunMetadata fields populated from opts_ once we wire identity propagation.)
    results_ =
        std::make_unique<bpt::backtester::results::ResultsCollector>(opts_.starting_capital, opts_.output_dir, md_meta);

    // HL decoder + publisher. Publisher fans out to BOTH the strategy
    // (via InProcessMdClient) AND the matching engine (via
    // bpt::backtester::data::MarketEvent), so resting LIMITs can be
    // filled by replayed book updates synchronously alongside strategy
    // notifications.
    hl_publisher_ = std::make_unique<HarnessMdPublisher>(*md_client_, &matching_, &refdata_client_->cache());
    hl_decoder_ = std::make_unique<bpt::md_gateway::adapter::HyperliquidMdDecoder<HarnessMdPublisher>>(hl_subs_);

    // Kick the strategy's lifecycle. start() typically sends the
    // refdata subscription request — InProcessRefdataClient fires
    // on_snapshot_complete + on_ready inline before subscribe()
    // returns, so by the time start() finishes the strategy has
    // its instrument set populated and is ready to receive MD.
    strategy_->start();

    // Strategy.start() ran on_snapshot synchronously, which prompted
    // the strategy to call md_client_->subscribe() with its target
    // instrument list. Now that the subscription is in hand, copy it
    // into the HL SubscriptionMap so the venue decoder knows which
    // canonical instrument_id each "coin" string maps to.
    for (const auto& desc : md_client_->subscribed_instruments()) {
        if (desc.exchange == "HYPERLIQUID") {
            hl_subs_.subscribe(desc.instrument_id, desc.symbol, desc.depth);
        }
    }
    bpt::common::log::info("[harness] subscribed {} HL instruments via venue decoder",
                           md_client_->subscribed_instruments().size());
}

uint64_t StrategyHarness::replay() {
    uint64_t last_ts = 0;
    for (const auto& path : opts_.wslog_paths) {
        WslogReader reader(path);
        if (!reader.ok()) {
            bpt::common::log::warn("[harness] failed to open wslog: {}", path);
            continue;
        }
        while (auto rec = reader.next()) {
            last_ts = rec->ts_ns;
            // Forward order-gateway clock ticks so heartbeats /
            // simulation_now reflect replay time, not wallclock.
            order_gw_->set_simulation_time(rec->ts_ns);

            if (rec->type == bpt::common::recorder::RecordType::WS_FRAME) {
                std::string_view payload(reinterpret_cast<const char*>(rec->payload.data()), rec->payload.size());
                // The decoder dispatches by HL channel ("l2Book" /
                // "trades" / "activeAssetCtx"), invokes hl_publisher_
                // for each parsed message — which fans out to both
                // matching engine and strategy synchronously.
                hl_decoder_->decode(payload, rec->ts_ns, *hl_publisher_, noop_funding_cb_);
            }
            // SESSION_*, CHECKPOINT, WS_DISCONNECT/RECONNECT records:
            // don't materially affect strategy output today (no
            // disconnect-replay logic in the harness yet — we treat
            // the tape as one continuous stream).
        }
    }
    return last_ts;
}

void StrategyHarness::finalize(uint64_t end_ts_ns) {
    if (hl_publisher_) {
        bpt::common::log::info("[harness] decoder produced {} trades + {} orderbooks for matching engine",
                               hl_publisher_->trade_count(),
                               hl_publisher_->orderbook_count());
    }
    // Tell the strategy to flatten any open positions. Multi-process
    // backtest does this via on_shutdown_flatten which sends IOC
    // unwind orders that get filled against the last book — same
    // path here, just synchronous.
    if (strategy_) {
        order_gw_->set_simulation_time(end_ts_ns);
        strategy_->on_shutdown_flatten();
    }
    if (results_) {
        results_->write();
        bpt::common::log::info("[harness] wrote results to {} (final equity reflects {} fills)",
                               opts_.output_dir,
                               "(see trades.csv)");
    }
}

void StrategyHarness::run() {
    initialize();
    const uint64_t end_ts = replay();
    finalize(end_ts);
}

}  // namespace bpt::backtester::harness
