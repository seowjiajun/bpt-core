#include "strategy/app/strategy_app.h"

#include "strategy/order/aeron_order_gateway_client.h"
#include "strategy/order/paper_order_gateway_client.h"
#include "strategy/strategy/strategy_factory.h"

#include <analytics/messaging/toxicity_update.h>

#include <messages/BacktestCommand.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/RejectReason.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/thread_pin.h>

using namespace std::chrono_literals;
using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::RefDataErrorType;
using bpt::messages::RejectReason;

namespace bpt::strategy {

StrategyApp::StrategyApp(config::AppConfig cfg,
                         std::shared_ptr<aeron::Aeron> aeron,
                         const bpt::common::util::Topology& topology)
    : cfg_(std::move(cfg)),
      aeron_(aeron),
      metrics_(cfg_.base.metrics_port),
      fee_cache_(cfg_.strat.strategy.schedule.max_refdata_staleness_ns),
      funding_rate_cache_(cfg_.strat.strategy.schedule.max_refdata_staleness_ns),
      topology_(topology) {
    const auto& ac = cfg_.aeron;
    const auto& fc = cfg_.strat;

    refdata_ = std::make_unique<refdata::RefdataClient>(aeron,
                                                        ac.refdata_control.channel,
                                                        ac.refdata_control.stream_id,
                                                        ac.refdata_snapshot.stream_id,
                                                        ac.refdata_delta.stream_id,
                                                        ac.fee_schedule.stream_id,
                                                        ac.funding_rate.stream_id,
                                                        ac.refdata_status.stream_id,
                                                        fee_cache_,
                                                        funding_rate_cache_);

    if (ac.md_control.stream_id != 0) {
        md_client_ = std::make_unique<md::MdClient>(aeron,
                                                    ac.md_control.channel,
                                                    ac.md_control.stream_id,
                                                    ac.md_data.stream_id,
                                                    ac.md_ack_hb.stream_id);
    }

    if (ac.order.stream_id != 0) {
        if (cfg_.strat.strategy.paper_mode) {
            // Canary / shadow run: swallow orders locally, synthesise
            // fills from the MD stream. Exchange never sees anything.
            // But the paper gateway DOES publish exec reports to the
            // live exec_report stream so bpt-bridge / dashboard see
            // paper fills identically to live ones — otherwise the
            // blotter + chart overlay are permanently empty in paper.
            auto paper = std::make_unique<order::PaperOrderGatewayClient>(
                aeron, ac.exec_report.channel, ac.exec_report.stream_id);
            paper_gw_ = paper.get();
            order_gw_ = std::move(paper);
            bpt::common::log::warn("================================================");
            bpt::common::log::warn(" PAPER MODE  —  orders will NOT reach the exchange ");
            bpt::common::log::warn("================================================");

            // Shadow aeron client for AccountSnapshotRequest only. Lets
            // the dashboard's Holdings panel show the real testnet
            // account state during a paper run — testnet capital is
            // fake, and hiding it produced empty UI with no diagnostic
            // signal. Orders/fills still flow through `paper_gw_`.
            snapshot_gw_ = std::make_unique<order::AeronOrderGatewayClient>(aeron,
                                                                    ac.order.channel,
                                                                    ac.order.stream_id,
                                                                    ac.exec_report.stream_id,
                                                                    ac.heartbeat.stream_id,
                                                                    ac.account_snapshot.stream_id);
        } else {
            order_gw_ = std::make_unique<order::AeronOrderGatewayClient>(aeron,
                                                                    ac.order.channel,
                                                                    ac.order.stream_id,
                                                                    ac.exec_report.stream_id,
                                                                    ac.heartbeat.stream_id,
                                                                    ac.account_snapshot.stream_id);
        }
    }

    if (ac.vol_surface.stream_id != 0) {
        vol_client_ = std::make_unique<vol::VolSurfaceClient>(aeron,
                                                              ac.vol_surface.channel,
                                                              ac.vol_surface.stream_id,
                                                              ac.pricer_status.stream_id);
        bpt::common::log::info("VolSurfaceClient ready: surface={} status={}",
                       ac.vol_surface.stream_id,
                       ac.pricer_status.stream_id);
    }

    if (ac.toxicity.stream_id != 0) {
        tyr_sub_ = bpt::common::aeron::wait_for_subscription(aeron, ac.toxicity.channel, ac.toxicity.stream_id);
        bpt::common::log::info("Analytics toxicity subscription ready: {} stream {}",
                       ac.toxicity.channel, ac.toxicity.stream_id);
    }

    if (order_gw_) {
        order_mgr_ = std::make_unique<order::OrderManager>(*order_gw_, refdata_->cache());
    }

    strategy_ = strategy::StrategyFactory::create(fc, *refdata_, md_client_.get(), order_mgr_.get(), vol_client_.get());

    // In paper mode, route AccountSnapshotRequest to the real aeron-backed
    // client (snapshot_gw_) so the real order-gateway answers; otherwise
    // the request would go to PaperOrderGatewayClient's no-op handler.
    auto* snap_client = snapshot_gw_ ? snapshot_gw_.get() : order_gw_.get();
    startup_gate_ = std::make_unique<app::StartupGate>(*refdata_,
                                                       snap_client,
                                                       *strategy_,
                                                       metrics_,
                                                       fc.strategy.schedule.configured_exchanges_mask,
                                                       fc.correlation_id);

    wire_refdata_callbacks();
    wire_md_callbacks();
    wire_vol_callbacks();
    wire_order_callbacks();

    if (cfg_.backtest_mode) {
        backtest_client_ =
            std::make_unique<backtest::BacktestClient>(aeron,
                                                       ac.backtest_control.channel,
                                                       ac.backtest_control.stream_id,  // sub: Backtester → Strategy
                                                       ac.backtest_ack.stream_id);     // pub: Strategy → Backtester
        bpt::common::log::info("Backtest mode enabled: ctrl_sub={} ack_pub={}",
                       ac.backtest_control.stream_id,
                       ac.backtest_ack.stream_id);
    }

    // Dashboard control subscription — halt/resume from the bridge.
    // 1-byte messages: 0x00 = HALT, 0x01 = RESUME.  Only enabled in
    // live/paper mode (not backtest — backtest has its own control channel).
    if (!cfg_.backtest_mode && ac.dashboard_control.stream_id != 0) {
        const int64_t reg_id = aeron->addSubscription(
            ac.dashboard_control.channel, ac.dashboard_control.stream_id);
        for (int i = 0; i < 500; ++i) {
            dashboard_ctrl_sub_ = aeron->findSubscription(reg_id);
            if (dashboard_ctrl_sub_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (dashboard_ctrl_sub_) {
            bpt::common::log::info("Dashboard control subscription ready on stream {}",
                           ac.dashboard_control.stream_id);
        } else {
            bpt::common::log::warn("Dashboard control subscription unavailable");
        }
    }

    // Portfolio snapshot publication — JSON-serialized strategy state for
    // the dashboard's options panels, throttled to ~10 Hz inside the
    // publisher. Disabled in backtest mode.
    if (!cfg_.backtest_mode) {
        portfolio_snap_pub_ = std::make_unique<dashboard::PortfolioSnapshotPublisher>(
            aeron, ac.dashboard_snapshot.channel, ac.dashboard_snapshot.stream_id);
    }
}

void StrategyApp::run() {
    // Pin the main poll loop to the topology's "strategy.main" role.
    // Strategy has no legacy io_cpu TOML knob, so unassigned → unpinned
    // (the helper logs at INFO, matching other services).
    bpt::common::util::pin_thread_by_role(topology_, "strategy.main", "strategy.main");

    // SIGUSR1 = emergency flatten without exit. Installed here (not in
    // main()) so the behaviour is scoped to strategies and doesn't
    // surprise other services that wouldn't know what to do with it.
    bpt::common::signal::install_flatten_handler();

    bpt::common::log::info("Polling... waiting for RefDataReady before subscribing (Ctrl+C to exit)");
    bpt::common::log::info("Emergency flatten: `kill -USR1 {}` (or systemctl --user kill --signal=SIGUSR1 bpt-strategy)",
                   ::getpid());

    while (bpt::common::signal::is_running()) {
        // SIGUSR1 → flip to halted state + run the shutdown flatten
        // once. Strategy stays alive after, so the dashboard / logs
        // can be inspected without the process exiting. Another
        // SIGUSR1 re-triggers (clears the flag on consumption).
        if (bpt::common::signal::is_flatten_requested()) {
            bpt::common::signal::clear_flatten_request();
            bpt::common::log::warn("EMERGENCY FLATTEN requested via SIGUSR1 — halting + draining");
            if (!trading_halted_) {
                trading_halted_ = true;
                metrics_.trading_halted->Set(1.0);
            }
            shutdown_flatten();
            bpt::common::log::warn("EMERGENCY FLATTEN complete — strategy stays halted. "
                           "To resume, restart the process (no resume path by design).");
        }

        int frags = refdata_->poll();
        if (md_client_)
            frags += md_client_->poll();
        if (vol_client_)
            frags += vol_client_->poll();
        if (order_gw_)
            frags += order_gw_->poll();
        if (snapshot_gw_)
            frags += snapshot_gw_->poll();
        if (tyr_sub_) {
            frags += tyr_sub_->poll(
                [this](const aeron::concurrent::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       const aeron::Header&) {
                    if (static_cast<std::size_t>(length) != sizeof(bpt::analytics::messaging::ToxicityUpdate))
                        return;
                    bpt::analytics::messaging::ToxicityUpdate update;
                    std::memcpy(&update, buffer.buffer() + offset, sizeof(update));
                    strategy_->on_toxicity_update(update);
                },
                4);
        }

        // Poll dashboard control channel (halt/resume from bridge)
        if (dashboard_ctrl_sub_) {
            frags += dashboard_ctrl_sub_->poll(
                [this](aeron::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       aeron::Header& /*hdr*/) {
                    if (length < 1) return;
                    const uint8_t cmd = *reinterpret_cast<const uint8_t*>(buffer.buffer() + offset);
                    if (cmd == 0x00 && !trading_halted_) {
                        trading_halted_ = true;
                        metrics_.trading_halted->Set(1.0);
                        bpt::common::log::warn("TRADING HALTED via dashboard kill-switch");
                    } else if (cmd == 0x01 && trading_halted_) {
                        trading_halted_ = false;
                        metrics_.trading_halted->Set(0.0);
                        bpt::common::log::info("Trading RESUMED via dashboard");
                    }
                },
                1);
        }

        startup_gate_->tick();

        // Once the strategy has its snapshot and started MD subscriptions, switch to
        // tick-gating mode if running a backtest.
        if (cfg_.backtest_mode && startup_gate_->is_active()) {
            run_backtest_loop();
            break;
        }

        if (startup_gate_->is_active()) {
            check_service_liveness();
            report_latency_stats();

            if (portfolio_snap_pub_ && portfolio_snap_pub_->is_active()) {
                const auto now_ns = bpt::common::util::TscClock::now_epoch_ns();
                portfolio_snap_pub_->publish_if_due(strategy_->get_portfolio_state(), now_ns);

                // Publish strategy state at ~2 Hz (every 500ms).
                static uint64_t last_state_ns = 0;
                if (now_ns - last_state_ns > 500'000'000ULL) {
                    last_state_ns = now_ns;
                    auto state_json = strategy_->get_strategy_state_json();
                    if (!state_json.empty()) {
                        aeron::AtomicBuffer buf(reinterpret_cast<uint8_t*>(state_json.data()),
                                                static_cast<aeron::util::index_t>(state_json.size()));
                        // Reuse the same publication as portfolio snapshots —
                        // the bridge relays all JSON on stream 9004 to WS clients.
                        portfolio_snap_pub_->offer_raw(buf, static_cast<int32_t>(state_json.size()));
                    }
                }
            }
        }

        if (frags == 0)
            __builtin_ia32_pause();
    }

}

void StrategyApp::stop() {
    // Called by bpt::app::run() after run() exits on signal. Graceful
    // shutdown does two things in order: (1) cancel resting orders and
    // flatten inventory while the dashboard can still observe, (2) drain
    // the Prometheus exposer. Both are symmetric with startup side-effects.
    shutdown_flatten();
    metrics_.shutdown();
}


void StrategyApp::check_service_liveness() {
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());

    // Throttle to once per second — no need to check every poll iteration.
    if (now_ns - last_liveness_check_ns_ < 1'000'000'000ULL)
        return;
    last_liveness_check_ns_ = now_ns;

    const uint64_t threshold_ns = cfg_.strat.strategy.schedule.md_staleness_threshold_ms * 1'000'000ULL;
    bool stale = false;

    if (md_client_ && last_md_hb_recv_ns_ != 0) {
        const uint64_t age_ns = now_ns - last_md_hb_recv_ns_;
        if (age_ns > threshold_ns) {
            bpt::common::log::warn("MD Gateway heartbeat stale ({:.1f}s, threshold={:.1f}s) — pausing trading",
                           age_ns / 1e9,
                           threshold_ns / 1e9);
            stale = true;
        }
    }

    if (order_gw_ && last_gw_hb_recv_ns_ != 0) {
        const uint64_t age_ns = now_ns - last_gw_hb_recv_ns_;
        if (age_ns > threshold_ns) {
            bpt::common::log::warn("OrderGateway heartbeat stale ({:.1f}s, threshold={:.1f}s) — pausing trading",
                           age_ns / 1e9,
                           threshold_ns / 1e9);
            stale = true;
        }
    }

    if (stale && !trading_paused_) {
        trading_paused_ = true;
        metrics_.trading_paused->Set(1.0);
        bpt::common::log::warn("Trading PAUSED — service heartbeat(s) stale");
    } else if (!stale && trading_paused_) {
        trading_paused_ = false;
        metrics_.trading_paused->Set(0.0);
        bpt::common::log::info("Trading RESUMED — all service heartbeats healthy");
    }
}

void StrategyApp::report_latency_stats() {
    constexpr uint64_t kReportIntervalNs = 30'000'000'000ULL;  // 30 s

    const uint64_t now = bpt::common::util::TscClock::now_epoch_ns();
    if (now - last_lat_report_ns_ < kReportIntervalNs)
        return;
    last_lat_report_ns_ = now;

    auto tick = tick_lat_hist_.snapshot_and_reset();
    auto ord = order_lat_hist_.snapshot_and_reset();

    if (tick.total == 0) {
        bpt::common::log::info("[Latency] No MD ticks processed in last 30s");
        return;
    }

    // T0 = bpt-md-gateway TSC at wire receipt; T3 = strategy TSC after strategy returns.
    // Both services calibrate TscClock independently — cross-process skew is
    // typically <1µs on a single host with invariant TSC, so delta is valid.
    bpt::common::log::info(
        "[Latency] MD tick→strategy (T0→T3): n={} "
        "p50={:.1f}µs p90={:.1f}µs p99={:.1f}µs p99.9={:.1f}µs max={:.1f}µs mean={:.1f}µs",
        tick.total,
        tick.percentile_ns(0.50) / 1e3,
        tick.percentile_ns(0.90) / 1e3,
        tick.percentile_ns(0.99) / 1e3,
        tick.percentile_ns(0.999) / 1e3,
        tick.max_ns() / 1e3,
        tick.mean_ns() / 1e3);

    if (ord.total > 0) {
        bpt::common::log::info(
            "[Latency] MD tick→order placed (T0→T3 w/order): n={} "
            "p50={:.1f}µs p90={:.1f}µs p99={:.1f}µs max={:.1f}µs mean={:.1f}µs",
            ord.total,
            ord.percentile_ns(0.50) / 1e3,
            ord.percentile_ns(0.90) / 1e3,
            ord.percentile_ns(0.99) / 1e3,
            ord.max_ns() / 1e3,
            ord.mean_ns() / 1e3);
    } else {
        bpt::common::log::info("[Latency] No orders placed in last 30s");
    }
}


}  // namespace bpt::strategy
