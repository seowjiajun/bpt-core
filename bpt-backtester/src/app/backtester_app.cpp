#include "backtester/app/backtester_app.h"

#include <messages/ExchangeRegistry.h>

#include <chrono>
#include <format>
#include <thread>
#include <bpt_common/aeron/aeron_utils.h>

namespace bpt::backtester {

BacktesterApp::BacktesterApp(config::Settings settings, std::shared_ptr<aeron::Aeron> aeron)
    : settings_(std::move(settings)), aeron_(std::move(aeron)) {
    bpt::common::log::info("Initialising — window: {} → {}, {} instrument(s)",
                   settings_.simulation.start,
                   settings_.simulation.end,
                   settings_.instruments.size());

    const auto& ac = settings_.aeron;

    auto ctrl_pub =
        bpt::common::aeron::wait_for_publication(aeron_, ac.backtest_control.channel, ac.backtest_control.stream_id);
    auto ack_sub =
        bpt::common::aeron::wait_for_subscription(aeron_, ac.backtest_ack.channel, ac.backtest_ack.stream_id);

    ctrl_pub_ = std::make_unique<messaging::BacktestControlPublisher>(std::move(ctrl_pub));
    ack_sub_ = std::make_unique<messaging::BacktestAckSubscriber>(std::move(ack_sub));

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

    hyperliquid_md_server_ =
        std::make_unique<exchange::HyperliquidMdServer>(settings_.endpoints.hyperliquid_md_port);
    hyperliquid_md_server_->start();

    // HL REST /info simulator. Loads refdata snapshot from disk and serves it
    // to bpt-refdata + bpt-order-gateway in backtest mode. Asset universe
    // populated from the snapshot is used to initialize HyperliquidOrderServer
    // so asset_idx → coin lookup works.
    hyperliquid_info_server_ = std::make_unique<exchange::HyperliquidInfoServer>(
        settings_.endpoints.hyperliquid_info_port,
        settings_.data.hyperliquid_refdata_snapshot,
        settings_.results.starting_capital);
    hyperliquid_info_server_->start();

    // ── Matching engine ────────────────────────────────────────────────────
    matching_engine_ = std::make_unique<matching::MatchingEngine>();

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
        bpt::common::log::warn("[BacktesterApp] HL asset universe empty from snapshot — "
                               "fell back to [[instruments]] config ({} entries)",
                               hl_universe.size());
    } else {
        bpt::common::log::info("[BacktesterApp] HL asset universe: {} coins from snapshot",
                               hl_universe.size());
    }
    hyperliquid_order_server_ = std::make_unique<exchange::HyperliquidOrderServer>(
        settings_.endpoints.hyperliquid_order_port, *matching_engine_, std::move(hl_universe));
    hyperliquid_order_server_->start();

    // ── Results collector ──────────────────────────────────────────────────
    std::string start_tag = settings_.simulation.start.substr(0, 10);
    std::string end_tag = settings_.simulation.end.substr(0, 10);

    results::ResultsCollector::RunMetadata metadata;
    metadata.simulation_start = settings_.simulation.start;
    metadata.simulation_end = settings_.simulation.end;
    metadata.instruments.reserve(settings_.instruments.size());
    for (const auto& inst : settings_.instruments)
        metadata.instruments.push_back(inst.exchange + ":" + inst.symbol);
    metadata.strategy_name = settings_.results.strategy_name;
    metadata.params_hash   = settings_.results.params_hash;
    metadata.git_sha       = settings_.results.git_sha;
    metadata.params_file   = settings_.results.params_file;

    const std::string run_id =
        results::ResultsCollector::compose_run_id(metadata, start_tag, end_tag);
    const std::string out_dir = std::format("{}/{}", settings_.results.output_dir, run_id);

    results_ = std::make_unique<results::ResultsCollector>(
        settings_.results.starting_capital, out_dir, std::move(metadata));

    matching_engine_->set_fill_callback([this](matching::FillReport fill) {
        results_->on_fill(fill);
        const auto exch_id = bpt::messages::ExchangeRegistry::from_name(fill.exchange);
        if (!exch_id) {
            bpt::common::log::warn("[BacktesterApp] Unknown exchange '{}' on fill — dropped", fill.exchange);
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
                bpt::common::log::warn("[BacktesterApp] No order server for exchange '{}' — fill dropped", fill.exchange);
                break;
        }
    });

    // ── ClockMaster ────────────────────────────────────────────────────────
    clock_master_ = std::make_unique<clock::ClockMaster>(*loader_,
                                                         binance_md_server_.get(),
                                                         okx_md_server_.get(),
                                                         hyperliquid_md_server_.get(),
                                                         matching_engine_.get(),
                                                         results_.get(),
                                                         ctrl_pub_.get(),
                                                         ack_sub_.get());

    bpt::common::log::info("Ready — results will be written to {}", out_dir);
}

void BacktesterApp::run() {
    // Wait for at least one MdGateway subscriber to connect before releasing data.
    // Without this gate, Backtester would exhaust all data before MdGateway has
    // had a chance to connect and subscribe.
    const uint32_t timeout_s = settings_.simulation.subscriber_wait_timeout_s;
    bpt::common::log::info("Waiting for subscriber (timeout={}s)...", timeout_s);

    uint32_t waited = 0;
    auto md_session_count = [this]() -> std::size_t {
        std::size_t n = 0;
        if (okx_md_server_) n += okx_md_server_->session_count();
        if (hyperliquid_md_server_) n += hyperliquid_md_server_->session_count();
        return n;
    };
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
