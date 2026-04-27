#include "order_gateway/app/gateway_app.h"

#include "order_gateway/adapter/binance/binance_order_adapter.h"
#include "order_gateway/adapter/deribit/deribit_order_adapter.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_order_adapter.h"
#include "order_gateway/adapter/okx/okx_order_adapter.h"

#include <messages/ExchangeId.h>
#include <messages/ExchangeRegistry.h>

#include <bpt_common/signal.h>
#include <bpt_common/util/thread_pin.h>
#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <x86intrin.h>

using namespace std::chrono_literals;
using bpt::messages::ExchangeId;

namespace bpt::order_gateway {

OrderGatewayApp::OrderGatewayApp(config::Settings cfg,
                                 std::shared_ptr<aeron::Aeron> aeron,
                                 std::map<std::string, adapter::ExchangeCredentials> creds,
                                 const bpt::common::util::Topology& topology)
    : cfg_(std::move(cfg)),
      aeron_(aeron),
      metrics_(cfg_.base.metrics_port),
      risk_checker_(cfg_.gateway.risk.max_order_size_usd,
                    cfg_.gateway.risk.max_notional_per_order_usd,
                    cfg_.gateway.risk.max_open_orders_per_venue,
                    cfg_.gateway.risk.max_orders_per_second),
      topology_(topology) {
    risk_checker_.set_trading_enabled(cfg_.gateway.risk.trading_enabled);

    exec_pub_ = std::make_shared<messaging::ExecReportPublisher>(aeron,
                                                                 cfg_.aeron.exec_report.channel,
                                                                 cfg_.aeron.exec_report.stream_id);
    hb_pub_ = std::make_shared<messaging::HeartbeatPublisher>(aeron,
                                                              cfg_.aeron.heartbeat.channel,
                                                              cfg_.aeron.heartbeat.stream_id);
    account_snap_pub_ = std::make_shared<messaging::AccountSnapshotPublisher>(aeron,
                                                                              cfg_.aeron.account_snapshot.channel,
                                                                              cfg_.aeron.account_snapshot.stream_id);
    order_sub_ =
        std::make_shared<messaging::OrderSubscriber>(aeron, cfg_.aeron.order.channel, cfg_.aeron.order.stream_id);

    // Shared per-adapter disconnect breaker config. Populated once,
    // applied to each adapter before start().
    risk::DisconnectRateBreaker::Config disc_cfg;
    disc_cfg.enabled = cfg_.gateway.risk.disconnect_breaker_enabled;
    disc_cfg.threshold = cfg_.gateway.risk.disconnect_threshold;
    disc_cfg.window_ns = static_cast<uint64_t>(cfg_.gateway.risk.disconnect_window_sec) * 1'000'000'000ULL;

    for (const auto& a_cfg : cfg_.gateway.adapters) {
        if (a_cfg.testnet)
            bpt::common::log::warn("*** TESTNET MODE *** adapter={} host={}",
                                   a_cfg.exchange,
                                   a_cfg.rest_host.empty() ? a_cfg.ws_host : a_cfg.rest_host);

        const auto& exchange_creds = [&]() -> const adapter::ExchangeCredentials& {
            static const adapter::ExchangeCredentials empty{};
            const auto it = creds.find(a_cfg.exchange);
            return it != creds.end() ? it->second : empty;
        }();

        const auto exch_id = bpt::messages::ExchangeRegistry::from_name(a_cfg.exchange);
        if (!exch_id) {
            throw std::runtime_error(fmt::format(
                "Unknown exchange '{}' in order-gateway config — not in messages/exchanges.yaml",
                a_cfg.exchange));
        }
        std::shared_ptr<adapter::IOrderAdapter> adapter;
        switch (*exch_id) {
            case bpt::messages::ExchangeId::BINANCE:
                adapter = std::make_shared<adapter::BinanceOrderAdapter>(a_cfg, exchange_creds);
                break;
            case bpt::messages::ExchangeId::OKX:
                adapter = std::make_shared<adapter::OKXOrderAdapter>(a_cfg, exchange_creds);
                break;
            case bpt::messages::ExchangeId::DERIBIT:
                adapter = std::make_shared<adapter::DeribitOrderAdapter>(a_cfg, exchange_creds);
                break;
            case bpt::messages::ExchangeId::HYPERLIQUID:
                adapter = std::make_shared<adapter::HyperliquidOrderAdapter>(a_cfg, exchange_creds);
                break;
            default:
                throw std::runtime_error(fmt::format(
                    "Exchange '{}' is in the registry but order-gateway has no adapter implementation for it",
                    a_cfg.exchange));
        }

        adapter->set_disconnect_breaker_config(disc_cfg);
        adapter->set_topology(topology_);
        adapter->start();
        adapters_.push_back(std::move(adapter));
        bpt::common::log::info("Started adapter: {}", a_cfg.exchange);
    }

    risk::RejectRateBreaker::Config breaker_cfg;
    breaker_cfg.enabled = cfg_.gateway.risk.reject_rate_breaker_enabled;
    breaker_cfg.threshold_pct = cfg_.gateway.risk.reject_rate_threshold_pct;
    breaker_cfg.window_ns = static_cast<uint64_t>(cfg_.gateway.risk.reject_rate_window_sec) * 1'000'000'000ULL;
    breaker_cfg.min_events = cfg_.gateway.risk.reject_rate_min_events;

    processor_ = std::make_unique<order::OrderProcessor>(*exec_pub_,
                                                         state_mgr_,
                                                         risk_checker_,
                                                         pnl_tracker_,
                                                         cfg_.gateway.risk.max_daily_loss_usd,
                                                         cfg_.gateway.risk.max_position_usd,
                                                         breaker_cfg,
                                                         metrics_,
                                                         adapters_);

    order_sub_->on_new_order = [this](const bpt::messages::NewOrder& o) {
        processor_->on_new_order(o);
    };
    order_sub_->on_cancel = [this](const bpt::messages::CancelOrder& c) {
        processor_->on_cancel(c);
    };
    order_sub_->on_cancel_all = [this](const bpt::messages::CancelAll& c) {
        processor_->on_cancel_all(c);
    };
    order_sub_->on_modify = [this](const bpt::messages::ModifyOrder& m) {
        processor_->on_modify(m);
    };
    order_sub_->on_account_snapshot_request = [this](const bpt::messages::AccountSnapshotRequest& req) {
        const auto exchange_id = req.exchangeId();
        const uint64_t correlation_id = req.correlationId();

        // Find the matching adapter and dispatch the blocking REST fetch to a
        // dedicated thread so the poll loop is not stalled.
        for (auto& adapter : adapters_) {
            if (adapter->exchange_id() != exchange_id)
                continue;
            std::thread([this, adapter, exchange_id, correlation_id]() {
                try {
                    auto snap = adapter->fetch_account_snapshot(correlation_id);
                    account_snap_pub_->publish(snap);
                } catch (const std::exception& e) {
                    bpt::common::log::error("AccountSnapshot fetch failed for exchange={}: {}",
                                            bpt::messages::ExchangeId::c_str(exchange_id),
                                            e.what());
                    // Publish empty snapshot so Strategy's gate doesn't hang.
                    adapter::AccountSnapshotData empty;
                    empty.exchange_id = exchange_id;
                    empty.correlation_id = correlation_id;
                    empty.timestamp_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                   std::chrono::system_clock::now().time_since_epoch())
                                                                   .count());
                    account_snap_pub_->publish(empty);
                }
            }).detach();
            return;
        }
        bpt::common::log::warn("AccountSnapshotRequest for unconfigured exchange={}",
                               bpt::messages::ExchangeId::c_str(exchange_id));
    };

    bpt::common::log::info("Ready — polling order stream {}", cfg_.aeron.order.stream_id);
}

void OrderGatewayApp::run() {
    bpt::common::util::TscClock::calibrate();
    // Pin the main poll loop. Prefer topology role "ogw.poll"; fall
    // back to legacy cfg_.gateway.poll_cpu knob when no role is set.
    if (!bpt::common::util::pin_thread_by_role(topology_, "ogw.poll", "poll"))
        bpt::common::util::pin_thread_to_cpu(cfg_.gateway.poll_cpu, "poll");

    const uint64_t hb_interval_ns = static_cast<uint64_t>(cfg_.gateway.heartbeat_interval_ms) * 1'000'000ULL;
    const uint64_t stale_timeout_ns = static_cast<uint64_t>(cfg_.gateway.stale_order_timeout_ms) * 1'000'000ULL;
    // Republish AccountSnapshot every 30s so late subscribers (e.g. the
    // dashboard bridge starting up after order-gateway) get a current view of
    // exchange balance without having to wait for a Strategy-driven request.
    // 5s interval keeps the dashboard HoldingsPanel fresh enough that a
    // manual flatten or other one-off order action is visible quickly,
    // without hammering the exchange REST API. Strategy's shutdown_flatten
    // path additionally sends an on-demand AccountSnapshotRequest to
    // trigger an immediate republish before the process exits.
    constexpr uint64_t kAccountSnapIntervalNs = 5'000'000'000ULL;
    uint64_t last_hb_ns = bpt::common::util::TscClock::now_epoch_ns();
    uint64_t last_account_snap_ns = 0;

    while (bpt::common::signal::is_running()) {
        // Drain exec events from all adapter SPSC queues first — lowest latency path.
        for (auto& a : adapters_)
            a->drain_exec_events([this](const adapter::ExecEvent& ev) { processor_->on_exec_event(ev); });

        int fragments = order_sub_->poll(10);

        const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

        if (now_ns - last_hb_ns >= hb_interval_ns) {
            uint8_t exchange_status = 0;
            for (const auto& a : adapters_) {
                const bool connected = a->is_connected();
                if (connected) {
                    switch (a->exchange_id()) {
                        case ExchangeId::BINANCE:
                            exchange_status |= 0x01;
                            break;
                        case ExchangeId::OKX:
                            exchange_status |= 0x02;
                            break;
                        case ExchangeId::HYPERLIQUID:
                            exchange_status |= 0x04;
                            break;
                        case ExchangeId::DERIBIT:
                            exchange_status |= 0x08;
                            break;
                        default:
                            break;
                    }
                }
                // Only emit a gauge for adapters actually configured in this
                // process — prevents phantom "disconnected" alerts for
                // venues a single-exchange deployment (e.g. bpt-ogw-okx)
                // was never wired to in the first place.
                metrics_.exchange_connected(a->exchange_name()).Set(connected ? 1.0 : 0.0);
            }
            const uint32_t open = state_mgr_.total_open_orders();
            const uint16_t open_orders = static_cast<uint16_t>(std::min<uint32_t>(open, 0xFFFF));
            hb_pub_->publish(1, open_orders, exchange_status);
            metrics_.open_orders->Set(static_cast<double>(open));

            // Risk / breaker gauge sampling — piggyback on the heartbeat
            // cadence so Prometheus has a fresh 0/1 edge every heartbeat
            // window. trading_enabled flipping to false is the daily-loss
            // latch; reject_rate_breaker_.tripped() latches on its own.
            metrics_.daily_loss_latched->Set(risk_checker_.trading_enabled() ? 0.0 : 1.0);
            metrics_.reject_rate_breaker_tripped->Set(processor_ && processor_->reject_rate_breaker_tripped() ? 1.0
                                                                                                              : 0.0);
            for (const auto& a : adapters_) {
                metrics_.disconnect_breaker_tripped(a->exchange_name()).Set(a->is_halted() ? 1.0 : 0.0);
            }

            processor_->check_stale_orders(stale_timeout_ns);

            last_hb_ns = now_ns;
        }

        // Periodic account snapshot republish — fetches are blocking REST
        // calls so dispatch to a detached thread (same pattern as the
        // on_account_snapshot_request handler).
        if (now_ns - last_account_snap_ns >= kAccountSnapIntervalNs) {
            last_account_snap_ns = now_ns;
            for (auto& adapter : adapters_) {
                if (!adapter->is_connected())
                    continue;
                auto a = adapter;  // capture the shared_ptr by value
                std::thread([this, a]() {
                    try {
                        auto snap = a->fetch_account_snapshot(/*correlation_id=*/0);
                        account_snap_pub_->publish(snap);
                    } catch (const std::exception& e) {
                        bpt::common::log::warn("Periodic AccountSnapshot fetch failed for {}: {}",
                                               bpt::messages::ExchangeId::c_str(a->exchange_id()),
                                               e.what());
                    }
                }).detach();
            }
        }

        if (fragments == 0)
            _mm_pause();
    }
}

void OrderGatewayApp::stop() {
    // Called by bpt::app::run() after the poll loop exits. Drains
    // adapter WS/REST threads + Prometheus exposer so teardown is
    // symmetric with the startup side-effects.
    for (auto& a : adapters_)
        a->stop();
    metrics_.shutdown();
}

}  // namespace bpt::order_gateway
