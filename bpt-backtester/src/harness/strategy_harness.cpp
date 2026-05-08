#include "backtester/harness/strategy_harness.h"

#include "backtester/harness/wslog_reader.h"

#include "strategy/strategy/strategy_factory.h"

#include <bpt_common/logging.h>

#include <stdexcept>

namespace bpt::backtester::harness {

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
    md_client_ = std::make_unique<bpt::strategy::md::InProcessMdClient>();

    // Load instrument mapping JSON into the refdata client's cache.
    // bpt-refdata's mapping_lib does the parsing — same loader the
    // production refdata service uses, so the harness's cache shape
    // is byte-identical to a live snapshot.
    bpt::refdata::mapping::InstrumentMappingLoader mapping;
    mapping.load(opts_.instrument_mapping_path);
    // TODO(harness): copy mapping entries into refdata_client_->mutable_cache().
    // Today this is a no-op stub — the cache stays empty, which means
    // the strategy's startup gate won't pass and on_snapshot is never
    // fired. Wire-up forthcoming when the wslog→event loop gets its
    // first end-to-end run; until then this whole binary builds but
    // does not produce useful output.

    // Strategy config — uses the same loader StrategyApp does, so
    // params land identically.
    strategy_cfg_ = bpt::strategy::config::AppConfig::load(opts_.strategy_config_path);

    // OrderManager wraps the order gateway + the refdata cache (used
    // for symbol → instrument resolution on send paths).
    order_mgr_ = std::make_unique<bpt::strategy::order::OrderManager>(
        *order_gw_, refdata_client_->cache());

    // Strategy.
    strategy_ = bpt::strategy::strategy::StrategyFactory::create(
        strategy_cfg_.strat,
        *refdata_client_,
        md_client_.get(),
        order_mgr_.get(),
        /*vol_client=*/nullptr);

    // Wire strategy callbacks. These mirror StrategyApp::wire_*_callbacks()
    // but are simpler because we don't need the latency histograms,
    // dashboard publish, or watchdog logic — those are integration
    // concerns that belong on the multi-process path.
    refdata_client_->on_snapshot_complete =
        [this](const bpt::strategy::refdata::InstrumentCache& cache) {
            strategy_->on_snapshot(cache);
        };
    refdata_client_->on_delta =
        [this](const bpt::strategy::refdata::Instrument& inst,
               bpt::messages::DeltaUpdateType::Value t) {
            strategy_->on_delta(inst, t);
        };
    md_client_->on_bbo = [this](const bpt::messages::MdMarketData& tick) {
        strategy_->on_bbo(tick);
    };
    md_client_->on_trade = [this](const bpt::messages::MdTrade& tick) {
        strategy_->on_trade(tick);
    };
    md_client_->on_order_book = [this](const bpt::messages::MdOrderBook& book) {
        strategy_->on_order_book(book);
    };
    order_gw_->on_exec_report = [this](const bpt::messages::ExecutionReport& rpt) {
        strategy_->on_exec_report(rpt);
    };

    // Results collector. Ownership of fees + run metadata follows the
    // same shape as ClockMaster's wiring in the multi-process path.
    bpt::backtester::results::ResultsCollector::RunMetadata md_meta{};
    // (RunMetadata fields populated from opts_ once we wire identity propagation.)
    results_ = std::make_unique<bpt::backtester::results::ResultsCollector>(
        opts_.starting_capital, opts_.output_dir, md_meta);

    // HL decoder + publisher — builds on the strategy's MD client.
    hl_publisher_ = std::make_unique<HarnessMdPublisher>(*md_client_);
    hl_decoder_ = std::make_unique<bpt::md_gateway::adapter::HyperliquidMdDecoder<HarnessMdPublisher>>(hl_subs_);

    // Kick the strategy's lifecycle. start() typically sends the
    // refdata subscription request — InProcessRefdataClient fires
    // on_snapshot_complete + on_ready inline before subscribe()
    // returns, so by the time start() finishes the strategy has
    // its instrument set populated and is ready to receive MD.
    strategy_->start();
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
                // TODO(harness): hand payload bytes to the venue
                // decoder. Currently the dispatch is a no-op stub —
                // it builds but doesn't decode. The decoder needs
                // the harness's HL SubscriptionMap populated to
                // resolve coin → instrument_id; that hook lands
                // in the next iteration alongside the cache load.
                std::string_view payload(
                    reinterpret_cast<const char*>(rec->payload.data()),
                    rec->payload.size());
                (void)payload;
            }
            // SESSION_*, CHECKPOINT, WS_DISCONNECT/RECONNECT records:
            // don't materially affect strategy output today (no
            // disconnect-replay logic in the harness yet — we treat
            // the tape as one continuous stream).
        }
    }
    return last_ts;
}

void StrategyHarness::finalize(uint64_t /*end_ts_ns*/) {
    // TODO(harness): shutdown-flatten + write summary.json.
    // For now the harness exits without writing results — the
    // path is staged for the next iteration once replay() actually
    // produces fills.
    if (results_) results_->write();
}

void StrategyHarness::run() {
    initialize();
    const uint64_t end_ts = replay();
    finalize(end_ts);
}

}  // namespace bpt::backtester::harness
