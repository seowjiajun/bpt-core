#include "backtester/app/backtester_app.h"

#include <chrono>
#include <format>
#include <thread>
#include <bpt_common/aeron/aeron_utils.h>

namespace bpt::backtester {

BacktesterApp::BacktesterApp(config::Settings settings, std::shared_ptr<aeron::Aeron> aeron)
    : settings_(std::move(settings)), aeron_(std::move(aeron)) {
    bpt::common::log::info("[Backtester] Initialising — window: {} → {}, {} instrument(s)",
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

    bpt::common::log::info("[Backtester] Backtest tick-gating ready: ctrl_pub=stream:{} ack_sub=stream:{}",
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

    // ── Matching engine ────────────────────────────────────────────────────
    matching_engine_ = std::make_unique<matching::MatchingEngine>();

    // ── Order WS/REST servers ──────────────────────────────────────────────
    binance_order_server_ =
        std::make_unique<exchange::BinanceOrderServer>(settings_.endpoints.binance_order_port, *matching_engine_);
    binance_order_server_->start();

    okx_order_server_ =
        std::make_unique<exchange::OkxOrderServer>(settings_.endpoints.okx_order_port, *matching_engine_);
    okx_order_server_->start();

    // ── Results collector ──────────────────────────────────────────────────
    std::string start_tag = settings_.simulation.start.substr(0, 10);
    std::string end_tag = settings_.simulation.end.substr(0, 10);
    std::string out_dir = std::format("{}/{}_{}", settings_.results.output_dir, start_tag, end_tag);

    results_ = std::make_unique<results::ResultsCollector>(settings_.results.starting_capital, out_dir);

    matching_engine_->set_fill_callback([this](matching::FillReport fill) {
        results_->on_fill(fill);
        if (fill.exchange == "BINANCE") {
            binance_order_server_->push_fill(fill);
        } else if (fill.exchange == "OKX") {
            okx_order_server_->push_fill(fill);
        } else {
            bpt::common::log::warn("[BacktesterApp] No order server for exchange '{}' — fill dropped", fill.exchange);
        }
    });

    // ── ClockMaster ────────────────────────────────────────────────────────
    clock_master_ = std::make_unique<clock::ClockMaster>(*loader_,
                                                         binance_md_server_.get(),
                                                         okx_md_server_.get(),
                                                         matching_engine_.get(),
                                                         results_.get(),
                                                         ctrl_pub_.get(),
                                                         ack_sub_.get());

    bpt::common::log::info("[Backtester] Ready — results will be written to {}", out_dir);
}

void BacktesterApp::run() {
    // Wait for at least one MdGateway subscriber to connect before releasing data.
    // Without this gate, Backtester would exhaust all data before MdGateway has
    // had a chance to connect and subscribe.
    const uint32_t timeout_s = settings_.simulation.subscriber_wait_timeout_s;
    bpt::common::log::info("[Backtester] Waiting for subscriber (timeout={}s)...", timeout_s);

    uint32_t waited = 0;
    while (okx_md_server_ && okx_md_server_->session_count() == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++waited >= timeout_s) {
            bpt::common::log::warn("[Backtester] No subscriber after {}s — starting anyway", timeout_s);
            break;
        }
    }

    if (okx_md_server_ && okx_md_server_->session_count() > 0)
        bpt::common::log::info("[Backtester] Subscriber connected, starting backtest");

    bpt::common::log::info("[Backtester] Starting backtest");
    clock_master_->run();
    results_->write();
    bpt::common::log::info("[Backtester] Backtest complete");
}

}  // namespace bpt::backtester
