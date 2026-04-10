#include "jormungandr/app/jormungandr_app.h"

#include <spdlog/spdlog.h>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/signal.h>

#include <chrono>
#include <format>
#include <stdexcept>

using namespace std::chrono_literals;

namespace jormungandr {

JormungandrApp::JormungandrApp(config::Settings settings, std::shared_ptr<aeron::Aeron> aeron)
    : settings_(std::move(settings)) {
    spdlog::info("[Jormungandr] Initialising — window: {} → {}, {} instrument(s)",
                 settings_.simulation.start, settings_.simulation.end,
                 settings_.instruments.size());

    // ── DataLoader ─────────────────────────────────────────────────────────
    loader_ = std::make_unique<data::DataLoader>(settings_.data, settings_.simulation,
                                                 settings_.instruments);
    loader_->validate();

    // ── MD WS servers ──────────────────────────────────────────────────────
    binance_md_server_ =
        std::make_unique<exchange::BinanceMdServer>(settings_.endpoints.binance_md_port);
    binance_md_server_->start();

    okx_md_server_ = std::make_unique<exchange::OkxMdServer>(settings_.endpoints.okx_md_port);
    okx_md_server_->start();

    // ── Matching engine ────────────────────────────────────────────────────
    matching_engine_ = std::make_unique<matching::MatchingEngine>();

    // ── Order WS/REST servers ──────────────────────────────────────────────
    binance_order_server_ = std::make_unique<exchange::BinanceOrderServer>(
        settings_.endpoints.binance_order_port, *matching_engine_);
    binance_order_server_->start();

    okx_order_server_ = std::make_unique<exchange::OkxOrderServer>(
        settings_.endpoints.okx_order_port, *matching_engine_);
    okx_order_server_->start();

    // ── Results collector ──────────────────────────────────────────────────
    // Compose output dir as "{configured_dir}/{start}_{end}" for easy organisation.
    std::string start_tag = settings_.simulation.start.substr(0, 10);  // YYYY-MM-DD
    std::string end_tag = settings_.simulation.end.substr(0, 10);
    std::string out_dir = std::format("{}/{}_{}", settings_.results.output_dir, start_tag, end_tag);

    results_ =
        std::make_unique<results::ResultsCollector>(settings_.results.starting_capital, out_dir);

    // Wire fill reports to the results collector and the appropriate exchange server.
    matching_engine_->set_fill_callback([this](matching::FillReport fill) {
        results_->on_fill(fill);
        if (fill.exchange == "BINANCE") {
            binance_order_server_->push_fill(fill);
        } else if (fill.exchange == "OKX") {
            okx_order_server_->push_fill(fill);
        } else {
            spdlog::warn("[JormungandrApp] No order server for exchange '{}' — fill dropped",
                         fill.exchange);
        }
    });

    // ── Aeron messaging ────────────────────────────────────────────────────
    const auto& aeron_cfg = settings_.aeron;

    auto ctrl_pub = ygg::aeron::wait_for_publication(aeron, aeron_cfg.backtest_control.channel,
                                                     aeron_cfg.backtest_control.stream_id);
    ctrl_pub_ = std::make_unique<messaging::BacktestControlPublisher>(std::move(ctrl_pub));

    auto ack_sub = ygg::aeron::wait_for_subscription(aeron, aeron_cfg.backtest_ack.channel,
                                                     aeron_cfg.backtest_ack.stream_id);
    ack_sub_ = std::make_unique<messaging::BacktestAckSubscriber>(std::move(ack_sub));

    // ── ClockMaster ────────────────────────────────────────────────────────
    constexpr auto kAckTimeout = 30'000ms;

    clock_master_ = std::make_unique<clock::ClockMaster>(
        *loader_, binance_md_server_.get(), okx_md_server_.get(), matching_engine_.get(),
        results_.get(), *ctrl_pub_, *ack_sub_, kAckTimeout);

    spdlog::info("[Jormungandr] Ready — results will be written to {}", out_dir);
}

void JormungandrApp::run() {
    spdlog::info("[Jormungandr] Starting backtest");
    clock_master_->run();
    results_->write();
    spdlog::info("[Jormungandr] Backtest complete");
}

}  // namespace jormungandr
