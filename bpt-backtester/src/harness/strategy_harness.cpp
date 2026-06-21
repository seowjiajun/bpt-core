#include "backtester/harness/strategy_harness.h"

#include "backtester/harness/wslog_reader.h"
#include "canon/canon_reader.h"
#include "canon/canon_sbe.h"
#include "strategy/strategy/strategy_factory.h"

#include <messages/ExchangeId.h>
#include <messages/ExchangeRegistry.h>

#include <bpt_common/logging.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

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

// Load the HL szDecimals snapshot that sits next to the instrument-mapping
// JSON (config/instruments/hyperliquid_meta.json). Returns coin -> szDecimals.
// Empty map if the file is absent — caller falls back to flat defaults.
//
// Deterministic by design: a committed static snapshot, NOT a live /info
// query, so a replay run is reproducible. Refresh the snapshot out-of-band
// when the HL universe changes (rare for established coins).
std::unordered_map<std::string, int> load_hl_sz_decimals(const std::string& mapping_path) {
    std::unordered_map<std::string, int> out;
    const auto meta_path = std::filesystem::path(mapping_path).parent_path() / "hyperliquid_meta.json";
    std::ifstream f(meta_path);
    if (!f) {
        bpt::common::log::warn("[StrategyHarness] no HL meta snapshot at {} — HL tick/lot fall back to flat defaults",
                               meta_path.string());
        return out;
    }
    try {
        const auto j = nlohmann::json::parse(f);
        for (const auto& [coin, sz] : j.at("szDecimals").items())
            out[coin] = sz.get<int>();
    } catch (const std::exception& e) {
        bpt::common::log::warn("[StrategyHarness] failed to parse HL meta snapshot: {}", e.what());
    }
    return out;
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

    // HL per-coin szDecimals — drives correct tick/lot per instrument
    // instead of one flat default for every HL perp. Without this, XMR
    // (lot 0.001) got the flat lot=1.0 and any sub-1-unit order_qty
    // rounded to zero → strategy posted no orders → zero fills.
    const auto hl_sz = load_hl_sz_decimals(opts_.instrument_mapping_path);

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
            // tick_size / lot_size: HL exposes its tick/lot rules via the
            // `/info` meta endpoint (szDecimals + pxDecimals per coin).
            // The mapping JSON we load doesn't carry those today, so seed
            // realistic defaults for HL perps — small enough to give the
            // strategy headroom on cheap coins (APE, DOGE) and large
            // enough on majors that AS's post-touch cap fires.
            //
            // Why this matters: AS's compute_quotes() gates the post-touch
            // cap on `tick_size > 0`. With tick_size == 0 the cap is
            // skipped entirely and the AS formula can produce quotes that
            // cross the book — strategy then quote-cycles forever
            // (cancel+replace on every BBO tick) without ever resting in
            // the book long enough to fill.
            //
            // Derive per-coin from the committed szDecimals snapshot, same
            // rule as the live HyperliquidRefdataDecoder:
            //   lot  = 10^-szDecimals
            //   tick = 10^-(6 - szDecimals)   (perp MAX_DECIMALS = 6)
            // Falls back to the old flat default if a coin isn't in the
            // snapshot. Note: the snapshot tick does NOT model HL's
            // 5-significant-figure price rule (which coarsens tick at high
            // price, e.g. BTC at $95k → ~$10 tick); for the backtest's
            // post-touch-cap + quote-rounding purposes the decimal-places
            // tick is an acceptable lower bound. Refine if a high-priced
            // coin's fills look unrealistic.
            if (venue.id == bpt::messages::ExchangeId::HYPERLIQUID) {
                const auto it = hl_sz.find(e.info.base);
                if (it != hl_sz.end()) {
                    const int sz = it->second;
                    inst.lot_size = std::pow(10.0, -sz);
                    inst.tick_size = std::pow(10.0, -std::max(0, 6 - sz));
                } else {
                    inst.tick_size = 0.0001;
                    inst.lot_size = 1.0;
                }
            } else if (venue.id == bpt::messages::ExchangeId::OKX) {
                // OKX V5 instrument metadata (tickSz / lotSz) varies per
                // contract. BTC-USDT-SWAP: tick=$0.1, lot=0.01 contracts
                // (each contract = 0.01 BTC face). Hardcoded for the
                // subset we backtest today; a real meta-snapshot loader
                // belongs on bpt-refdata's OKX adapter.
                if (e.venue_symbol == "BTC-USDT-SWAP") {
                    inst.tick_size = 0.1;
                    inst.lot_size = 0.01;
                }
            }
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
    // Pipe matching-engine fills into the results collector. Without
    // this every FILLED ExecReport reaches the strategy (P&L tracker)
    // but summary.json / trades.csv stay empty — collector aggregates
    // off the FillReport stream, not ExecReports.
    order_gw_->set_results_collector(results_.get());

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
    // Populate two tracking sets:
    //   - hl_subs_ is the HL JSON decoder's subscription map, only used
    //     by the wslog replay path. Limit it to HL entries.
    //   - strategy_instrument_ids_ is venue-agnostic — used by the canon
    //     replay path to filter SBE events to what the strategy actually
    //     subscribed to. Add every venue here so OKX / future venues
    //     work without code edits.
    for (const auto& desc : md_client_->subscribed_instruments()) {
        if (desc.exchange == "HYPERLIQUID")
            hl_subs_.subscribe(desc.instrument_id, desc.symbol, desc.depth);
        strategy_instrument_ids_.insert(desc.instrument_id);
    }
    bpt::common::log::info("[harness] subscribed {} instruments ({} HL via JSON decoder)",
                           md_client_->subscribed_instruments().size(),
                           hl_subs_.snapshot().size());
}

uint64_t StrategyHarness::replay() {
    if (!opts_.canon_paths.empty())
        return replay_canon();
    return replay_wslog();
}

uint64_t StrategyHarness::replay_wslog() {
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
            // Same for refdata: without a fresh heartbeat the strategy's
            // RefdataStaleGate trips after ~5s of replay time and the
            // entire on_bbo path early-returns, silencing the quoter for
            // the rest of the run. There's no live refdata producer in
            // backtest, so we synthesize the heartbeat from the same
            // tape timestamp.
            refdata_client_->push_heartbeat(rec->ts_ns);

            if (rec->type == bpt::common::recorder::RecordType::WS_FRAME) {
                std::string_view payload(reinterpret_cast<const char*>(rec->payload.data()), rec->payload.size());
                // The decoder dispatches by HL channel ("l2Book" /
                // "trades" / "activeAssetCtx"), invokes hl_publisher_
                // for each parsed message — which fans out to both
                // matching engine and strategy synchronously.
                hl_decoder_->decode(payload, rec->ts_ns, *hl_publisher_, noop_funding_cb_, noop_stats_cb_);
            }
            // SESSION_*, CHECKPOINT, WS_DISCONNECT/RECONNECT records:
            // don't materially affect strategy output today (no
            // disconnect-replay logic in the harness yet — we treat
            // the tape as one continuous stream).
        }
    }
    return last_ts;
}

uint64_t StrategyHarness::replay_canon() {
    // Canon source: skip the JSON decoder, decode each canon record's
    // SBE blob back to the same MdBbo / MdTrade / MdOrderBook /
    // FundingRateUpdate domain types the wslog path produces, and
    // dispatch through the same HarnessMdPublisher so the matching
    // engine + strategy fan-out is byte-equivalent to wslog replay.
    //
    // Filter by strategy_instrument_ids_: canon files are produced by
    // bpt-canon-replay against the full venue universe (so they're
    // generic across strategies), but the live wslog path filters
    // events to the strategy's subscribed set inside the HL decoder via
    // SubscriptionMap. Without this filter, non-APE BBOs would bump
    // simulation_time / refdata heartbeats and shift AS's time-based
    // state — that was the cause of the wslog/canon parity drift.
    bpt::common::log::info("[harness] replay_canon: strategy_instrument_ids_ has {} entries",
                           strategy_instrument_ids_.size());
    uint64_t bbo_kept = 0, bbo_filtered = 0, trade_kept = 0, trade_filtered = 0;
    uint64_t last_ts = 0;
    for (const auto& path : opts_.canon_paths) {
        bpt::canon::CanonReader reader(path);
        if (!reader.ok()) {
            bpt::common::log::warn("[harness] failed to open canon: {}", path);
            continue;
        }
        while (auto rec = reader.next()) {
            // Advance clock + heartbeat on EVERY record, including
            // events for instruments the strategy doesn't subscribe to.
            // This mirrors the wslog harness loop, which ticks the
            // clock on every wslog record before deciding whether to
            // dispatch the frame to the strategy.
            last_ts = rec->ts_ns;
            order_gw_->set_simulation_time(rec->ts_ns);
            refdata_client_->push_heartbeat(rec->ts_ns);

            const char* sbe_buf = reinterpret_cast<const char*>(rec->sbe.data());
            const std::size_t sbe_len = rec->sbe.size();

            switch (rec->type) {
                case bpt::canon::EventType::BBO: {
                    bpt::md_gateway::md::MdBbo bbo{};
                    if (!bpt::canon::decode_bbo(sbe_buf, sbe_len, bbo))
                        break;
                    // Strategy-subscription filter — same as wslog HL
                    // decoder's `subs_.find_id(coin) == 0 → return`.
                    if (!strategy_instrument_ids_.contains(bbo.instrument_id)) {
                        ++bbo_filtered;
                        break;
                    }
                    ++bbo_kept;
                    bbo.timestamp_ns = rec->ts_ns;
                    hl_publisher_->publish(bbo);
                    break;
                }
                case bpt::canon::EventType::TRADE: {
                    bpt::md_gateway::md::MdTrade trade{};
                    if (!bpt::canon::decode_trade(sbe_buf, sbe_len, trade))
                        break;
                    if (!strategy_instrument_ids_.contains(trade.instrument_id)) {
                        ++trade_filtered;
                        break;
                    }
                    ++trade_kept;
                    trade.timestamp_ns = rec->ts_ns;
                    hl_publisher_->publish(trade);
                    break;
                }
                case bpt::canon::EventType::BOOK: {
                    bpt::md_gateway::md::MdOrderBook book{};
                    if (!bpt::canon::decode_book(sbe_buf, sbe_len, book))
                        break;
                    if (!strategy_instrument_ids_.contains(book.instrument_id))
                        break;
                    book.timestamp_ns = rec->ts_ns;
                    hl_publisher_->publish(book);
                    break;
                }
                case bpt::canon::EventType::FUNDING:
                case bpt::canon::EventType::MARK:
                    // AS strategy doesn't react to funding/mark directly —
                    // see noop_funding_cb_ on the wslog path.
                    break;
            }
        }
    }
    bpt::common::log::info("[harness] replay_canon stats: BBO kept={} filtered={} | Trade kept={} filtered={}",
                           bbo_kept,
                           bbo_filtered,
                           trade_kept,
                           trade_filtered);
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
