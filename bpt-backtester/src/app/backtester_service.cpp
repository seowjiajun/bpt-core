#include "backtester/app/backtester_service.h"

#include <messages/ExchangeRegistry.h>

#include <chrono>
#include <format>
#include <thread>

namespace bpt::backtester {

namespace {

// Build the latency model if the [simulation.latency] block had any
// non-zero field; otherwise return nullptr. Returning nullptr is the
// signal MatchingEngine uses to fall back to its pre-Phase-3 zero-latency
// path, so a config without a [simulation.latency] section keeps the old
// behaviour unchanged.
std::unique_ptr<latency::ParametricLatencyModel> build_latency_model(const config::SimLatencyConfig& cfg) {
    auto has_nonzero = [](const config::VenueLatencySpec& s) {
        return s.submit_to_match_base_ns > 0 || s.submit_to_match_jitter_ns > 0 || s.match_to_report_base_ns > 0 ||
               s.match_to_report_jitter_ns > 0;
    };
    const bool any_spec = !cfg.per_venue.empty() || has_nonzero(cfg.default_spec);
    if (!any_spec)
        return nullptr;

    auto model = std::make_unique<latency::ParametricLatencyModel>(cfg.seed);
    using Leg = latency::LatencyLeg;
    model->set_default(Leg::SUBMIT_TO_MATCH,
                       {cfg.default_spec.submit_to_match_base_ns, cfg.default_spec.submit_to_match_jitter_ns});
    model->set_default(Leg::MATCH_TO_REPORT,
                       {cfg.default_spec.match_to_report_base_ns, cfg.default_spec.match_to_report_jitter_ns});
    for (const auto& [venue, spec] : cfg.per_venue) {
        model->set_spec(venue, Leg::SUBMIT_TO_MATCH, {spec.submit_to_match_base_ns, spec.submit_to_match_jitter_ns});
        model->set_spec(venue, Leg::MATCH_TO_REPORT, {spec.match_to_report_base_ns, spec.match_to_report_jitter_ns});
    }
    bpt::common::log::info("[BacktesterService] LatencyModel installed (seed={}, {} venue overrides)",
                           cfg.seed,
                           cfg.per_venue.size());
    return model;
}

}  // namespace

BacktesterService::BacktesterService(config::Settings settings, messaging::BacktesterBus bus)
    : settings_(std::move(settings)),
      bus_(std::move(bus)) {
    bpt::common::log::info("Initialising — window: {} → {}, {} instrument(s)",
                           settings_.simulation.start,
                           settings_.simulation.end,
                           settings_.instruments.size());

    const auto& ac = settings_.aeron;
    bpt::common::log::info("Backtest tick-gating ready: ctrl_pub=stream:{} ack_sub=stream:{}",
                           ac.backtest_control.stream_id,
                           ac.backtest_ack.stream_id);

    // ── DataLoader ─────────────────────────────────────────────────────────
    loader_ = std::make_unique<data::DataLoader>(settings_.data, settings_.simulation, settings_.instruments);
    loader_->validate();

    // ── MD WS servers ──────────────────────────────────────────────────────
    binance_md_server_ = std::make_unique<exchange::BinanceMdServer>(settings_.endpoints.binance_md_port);
    binance_md_server_->start();

    okx_md_server_ = std::make_unique<exchange::OkxMdServer>(settings_.endpoints.okx_md_port);
    okx_md_server_->start();

    hyperliquid_md_server_ = std::make_unique<exchange::HyperliquidMdServer>(settings_.endpoints.hyperliquid_md_port);
    hyperliquid_md_server_->start();

    // HL REST /info simulator. Loads refdata snapshot from disk and serves it
    // to bpt-refdata + bpt-order-gateway in backtest mode. Asset universe
    // populated from the snapshot is used to initialize HyperliquidOrderServer
    // so asset_idx → coin lookup works.
    hyperliquid_info_server_ =
        std::make_unique<exchange::HyperliquidInfoServer>(settings_.endpoints.hyperliquid_info_port,
                                                          settings_.data.hyperliquid_refdata_snapshot,
                                                          settings_.results.starting_capital);
    hyperliquid_info_server_->start();

    // ── Latency model + matching engine ────────────────────────────────────
    // latency_model_ must outlive matching_engine_ — engine holds a
    // non-owning pointer. nullptr is the signal for "no latency simulation".
    latency_model_ = build_latency_model(settings_.simulation.latency);
    matching_engine_ = std::make_unique<matching::MatchingEngine>();
    if (latency_model_)
        matching_engine_->set_latency_model(latency_model_.get());

    // ── Order WS/REST servers ──────────────────────────────────────────────
    binance_order_server_ =
        std::make_unique<exchange::BinanceOrderServer>(settings_.endpoints.binance_order_port, *matching_engine_);
    binance_order_server_->start();

    okx_order_server_ =
        std::make_unique<exchange::OkxOrderServer>(settings_.endpoints.okx_order_port, *matching_engine_);
    okx_order_server_->start();

    // HL asset universe: prefer the snapshot served by HyperliquidInfoServer
    // (matches HL's real asset_idx ordering). Fall back to [[instruments]]
    // config for older configs that don't ship a snapshot path.
    std::vector<std::string> hl_universe = hyperliquid_info_server_->asset_universe();
    if (hl_universe.empty()) {
        for (const auto& inst : settings_.instruments) {
            if (inst.exchange == "HYPERLIQUID")
                hl_universe.push_back(inst.symbol);
        }
        bpt::common::log::warn(
            "[BacktesterService] HL asset universe empty from snapshot — "
            "fell back to [[instruments]] config ({} entries)",
            hl_universe.size());
    } else {
        bpt::common::log::info("[BacktesterService] HL asset universe: {} coins from snapshot", hl_universe.size());
    }
    hyperliquid_order_server_ =
        std::make_unique<exchange::HyperliquidOrderServer>(settings_.endpoints.hyperliquid_order_port,
                                                           *matching_engine_,
                                                           std::move(hl_universe));
    hyperliquid_order_server_->start();

    // ── Results collector ──────────────────────────────────────────────────
    const std::string start_tag = settings_.simulation.start.substr(0, 10);
    const std::string end_tag = settings_.simulation.end.substr(0, 10);

    results::ResultsCollector::RunMetadata metadata;
    metadata.simulation_start = settings_.simulation.start;
    metadata.simulation_end = settings_.simulation.end;
    metadata.instruments.reserve(settings_.instruments.size());
    for (const auto& inst : settings_.instruments)
        metadata.instruments.push_back(inst.exchange + ":" + inst.symbol);
    metadata.strategy_name = settings_.results.strategy_name;
    metadata.params_hash = settings_.results.params_hash;
    metadata.git_sha = settings_.results.git_sha;
    metadata.params_file = settings_.results.params_file;

    const std::string run_id = results::ResultsCollector::compose_run_id(metadata, start_tag, end_tag);
    const std::string out_dir = std::format("{}/{}", settings_.results.output_dir, run_id);

    results_ = std::make_unique<results::ResultsCollector>(settings_.results.starting_capital,
                                                           out_dir,
                                                           std::move(metadata),
                                                           settings_.results.fees_by_venue);

    matching_engine_->set_fill_callback([this](matching::FillReport fill) { on_fill(fill); });

    // ── ClockMaster ────────────────────────────────────────────────────────
    clock_master_ = std::make_unique<clock::ClockMaster>(*loader_,
                                                         binance_md_server_.get(),
                                                         okx_md_server_.get(),
                                                         hyperliquid_md_server_.get(),
                                                         matching_engine_.get(),
                                                         results_.get(),
                                                         bus_.ctrl_pub.get(),
                                                         bus_.ack_sub.get());

    bpt::common::log::info("Ready — results will be written to {}", out_dir);
}

void BacktesterService::on_fill(const matching::FillReport& fill) {
    results_->on_fill(fill);
    const auto exch_id = bpt::messages::ExchangeRegistry::from_name(fill.exchange);
    if (!exch_id) {
        bpt::common::log::warn("[BacktesterService] Unknown exchange '{}' on fill — dropped", fill.exchange);
        return;
    }
    switch (*exch_id) {
        case bpt::messages::ExchangeId::BINANCE:
            binance_order_server_->push_fill(fill);
            break;
        case bpt::messages::ExchangeId::OKX:
            okx_order_server_->push_fill(fill);
            break;
        case bpt::messages::ExchangeId::HYPERLIQUID:
            hyperliquid_order_server_->push_fill(fill);
            break;
        default:
            bpt::common::log::warn("[BacktesterService] No order server for exchange '{}' — fill dropped", fill.exchange);
            break;
    }
}

void BacktesterService::run() {
    // Wait for at least one MdGateway subscriber to connect before releasing data.
    // Without this gate, Backtester would exhaust all data before MdGateway has
    // had a chance to connect and subscribe.
    const uint32_t timeout_s = settings_.simulation.subscriber_wait_timeout_s;
    bpt::common::log::info("Waiting for subscriber (timeout={}s)...", timeout_s);

    // Sum subscribers across every venue's MD server — a single backtest can be
    // driven by mdgw connected to any subset. Previously only OKX + HL were
    // counted, which let Binance-only backtests proceed past the timeout
    // without an actual subscriber attached.
    auto md_session_count = [this]() -> std::size_t {
        std::size_t n = 0;
        if (binance_md_server_)
            n += binance_md_server_->session_count();
        if (okx_md_server_)
            n += okx_md_server_->session_count();
        if (hyperliquid_md_server_)
            n += hyperliquid_md_server_->session_count();
        return n;
    };

    uint32_t waited = 0;
    while (md_session_count() == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++waited >= timeout_s) {
            bpt::common::log::warn("No subscriber after {}s — starting anyway", timeout_s);
            break;
        }
    }

    if (md_session_count() > 0)
        bpt::common::log::info("Subscriber connected, starting backtest");

    bpt::common::log::info("Starting backtest");
    clock_master_->run();
    results_->write();
    bpt::common::log::info("Backtest complete");
}

}  // namespace bpt::backtester
