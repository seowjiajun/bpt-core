#include "order_gateway/app/gateway_app.h"

#include "order_gateway/adapter/binance/binance_order_adapter.h"
#include "order_gateway/adapter/deribit/deribit_order_adapter.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_order_adapter.h"
#include "order_gateway/adapter/okx/okx_order_adapter.h"

#include <messages/ExchangeId.h>

#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <x86intrin.h>
#include <yggdrasil/signal.h>
#include <yggdrasil/util/thread_pin.h>
#include <yggdrasil/util/tsc_clock.h>

using namespace std::chrono_literals;
using bpt::messages::ExchangeId;

namespace bpt::order_gateway {

Order GatewayApp::Order GatewayApp(config::Settings cfg,
                         std::shared_ptr<aeron::Aeron> aeron,
                         std::map<std::string, adapter::ExchangeCredentials> creds)
    : cfg_(std::move(cfg)),
      aeron_(aeron),
      metrics_(cfg_.metrics_port),
      risk_checker_(cfg_.gateway.risk.max_order_size_usd,
                    cfg_.gateway.risk.max_notional_per_order_usd,
                    cfg_.gateway.risk.max_open_orders_per_venue,
                    cfg_.gateway.risk.max_orders_per_second) {
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

    for (const auto& a_cfg : cfg_.gateway.adapters) {
        if (a_cfg.testnet)
            ygg::log::warn("[Order Gateway] *** TESTNET MODE *** adapter={} host={}",
                           a_cfg.exchange,
                           a_cfg.rest_host.empty() ? a_cfg.ws_host : a_cfg.rest_host);

        const auto& exchange_creds = [&]() -> const adapter::ExchangeCredentials& {
            static const adapter::ExchangeCredentials empty{};
            const auto it = creds.find(a_cfg.exchange);
            return it != creds.end() ? it->second : empty;
        }();

        std::shared_ptr<adapter::IOrderAdapter> adapter;
        if (a_cfg.exchange == "BINANCE") {
            adapter = std::make_shared<adapter::BinanceOrderAdapter>(a_cfg, exchange_creds);
        } else if (a_cfg.exchange == "OKX") {
            adapter = std::make_shared<adapter::OKXOrderAdapter>(a_cfg, exchange_creds);
        } else if (a_cfg.exchange == "DERIBIT") {
            adapter = std::make_shared<adapter::DeribitOrderAdapter>(a_cfg, exchange_creds);
        } else if (a_cfg.exchange == "HYPERLIQUID") {
            adapter = std::make_shared<adapter::HyperliquidOrderAdapter>(a_cfg, exchange_creds);
        } else {
            ygg::log::warn("[Order Gateway] Unknown exchange in config: {}", a_cfg.exchange);
            continue;
        }

        adapter->start();
        adapters_.push_back(std::move(adapter));
        ygg::log::info("[Order Gateway] Started adapter: {}", a_cfg.exchange);
    }

    processor_ = std::make_unique<order::OrderProcessor>(*exec_pub_, state_mgr_, risk_checker_, metrics_, adapters_);

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
                    ygg::log::error("[Order Gateway] AccountSnapshot fetch failed for exchange={}: {}",
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
        ygg::log::warn("[Order Gateway] AccountSnapshotRequest for unconfigured exchange={}",
                       bpt::messages::ExchangeId::c_str(exchange_id));
    };

    ygg::log::info("[Order Gateway] Ready — polling order stream {}", cfg_.aeron.order.stream_id);
}

void Order GatewayApp::run() {
    ygg::util::TscClock::calibrate();
    ygg::util::pin_thread_to_cpu(cfg_.gateway.poll_cpu, "poll");

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
    uint64_t last_hb_ns = ygg::util::TscClock::now_epoch_ns();
    uint64_t last_account_snap_ns = 0;

    while (ygg::signal::is_running()) {
        // Drain exec events from all adapter SPSC queues first — lowest latency path.
        for (auto& a : adapters_)
            a->drain_exec_events([this](const adapter::ExecEvent& ev) { processor_->on_exec_event(ev); });

        int fragments = order_sub_->poll(10);

        const uint64_t now_ns = ygg::util::TscClock::now_epoch_ns();

        if (now_ns - last_hb_ns >= hb_interval_ns) {
            uint8_t exchange_status = 0;
            for (const auto& a : adapters_) {
                if (a->exchange_id() == ExchangeId::BINANCE && a->is_connected())
                    exchange_status |= 0x01;
                if (a->exchange_id() == ExchangeId::OKX && a->is_connected())
                    exchange_status |= 0x02;
                if (a->exchange_id() == ExchangeId::HYPERLIQUID && a->is_connected())
                    exchange_status |= 0x04;
                if (a->exchange_id() == ExchangeId::DERIBIT && a->is_connected())
                    exchange_status |= 0x08;
            }
            const uint32_t open = state_mgr_.total_open_orders();
            const uint16_t open_orders = static_cast<uint16_t>(std::min<uint32_t>(open, 0xFFFF));
            hb_pub_->publish(1, open_orders, exchange_status);
            metrics_.exchange_connected("BINANCE").Set((exchange_status & 0x01) ? 1.0 : 0.0);
            metrics_.exchange_connected("OKX").Set((exchange_status & 0x02) ? 1.0 : 0.0);
            metrics_.exchange_connected("HYPERLIQUID").Set((exchange_status & 0x04) ? 1.0 : 0.0);
            metrics_.exchange_connected("DERIBIT").Set((exchange_status & 0x08) ? 1.0 : 0.0);
            metrics_.open_orders->Set(static_cast<double>(open));

            processor_->check_stale_orders(stale_timeout_ns);

            last_hb_ns = now_ns;
        }

        // Periodic account snapshot republish — fetches are blocking REST
        // calls so dispatch to a detached thread (same pattern as the
        // on_account_snapshot_request handler).
        if (now_ns - last_account_snap_ns >= kAccountSnapIntervalNs) {
            last_account_snap_ns = now_ns;
            for (auto& adapter : adapters_) {
                if (!adapter->is_connected()) continue;
                auto a = adapter;  // capture the shared_ptr by value
                std::thread([this, a]() {
                    try {
                        auto snap = a->fetch_account_snapshot(/*correlation_id=*/0);
                        account_snap_pub_->publish(snap);
                    } catch (const std::exception& e) {
                        ygg::log::warn("[Order Gateway] Periodic AccountSnapshot fetch failed for {}: {}",
                                       bpt::messages::ExchangeId::c_str(a->exchange_id()),
                                       e.what());
                    }
                }).detach();
            }
        }

        if (fragments == 0)
            _mm_pause();
    }

    for (auto& a : adapters_)
        a->stop();

    metrics_.shutdown();
    ygg::log::info("[Order Gateway] Shutting down");
}

}  // namespace bpt::order_gateway
