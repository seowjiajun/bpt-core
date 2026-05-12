#include "strategy/app/strategy_app.h"

#include "strategy/strategy/strategy_factory.h"

#include <messages/BacktestCommand.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/RejectReason.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/thread_pin.h>

using namespace std::chrono_literals;
using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::RefDataErrorType;
using bpt::messages::RejectReason;

namespace bpt::strategy {

StrategyApp::StrategyApp(config::AppConfig cfg,
                         messaging::StrategyBus bus,
                         const bpt::common::util::Topology& topology)
    : cfg_(std::move(cfg)),
      metrics_(cfg_.base.metrics_port),
      bus_(std::move(bus)),
      refdata_stale_gate_({
          .startup_timeout_ns = cfg_.strat.strategy.schedule.startup_refdata_timeout_ns,
          .stale_threshold_ns = cfg_.strat.strategy.schedule.refdata_heartbeat_timeout_ns,
      }),
      topology_(topology) {
    const auto& fc = cfg_.strat;

    if (bus_.order_gw) {
        order_mgr_ = std::make_unique<order::OrderManager>(*bus_.order_gw, bus_.refdata->cache());
    }

    strategy_ = strategy::StrategyFactory::create(fc, *bus_.refdata, bus_.md.get(), order_mgr_.get(), bus_.vol.get());

    startup_gate_ = std::make_unique<app::StartupGate>(*bus_.refdata,
                                                       bus_.order_gw.get(),
                                                       *strategy_,
                                                       metrics_,
                                                       fc.strategy.schedule.configured_exchanges_mask,
                                                       fc.correlation_id);

    wire_refdata_callbacks();
    wire_md_callbacks();
    wire_vol_callbacks();
    wire_order_callbacks();
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

    bpt::common::log::info("Polling... waiting for RefDataReady before subscribing");
    bpt::common::log::info("Emergency flatten: `kill -USR1 {}` (or systemctl --user kill --signal=SIGUSR1 bpt-strategy)",
                   ::getpid());

    // Anchor for the refdata startup timeout. If no heartbeat arrives
    // within startup_refdata_timeout_ns, check_service_liveness() will
    // exit the process — replaces the previous hang-forever behaviour.
    //
    // Must use TscClock (wall-epoch ns), NOT steady_clock — the
    // heartbeat we compare against is published with TscClock by
    // bpt-refdata's RefdataDeltaPublisher. Mixing clocks here produces
    // a garbage stale check that fires immediately at startup.
    startup_anchor_ns_ = bpt::common::util::TscClock::now_epoch_ns();
    refdata_stale_gate_.set_started_at(startup_anchor_ns_);

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

        int frags = bus_.refdata->poll();
        if (bus_.md)
            frags += bus_.md->poll();
        if (bus_.vol)
            frags += bus_.vol->poll();
        if (bus_.order_gw)
            frags += bus_.order_gw->poll();
        if (bus_.tox) {
            // Bind the callback once on first iteration (cheap, idempotent).
            // Doing it here keeps the strategy_->on_toxicity_update target
            // co-located with the poll site; the alternative is wiring it
            // in the constructor, which spreads tox plumbing across files.
            if (!bus_.tox->on_update) {
                bus_.tox->on_update = [this](const bpt::analytics::messaging::ToxicityUpdate& u) {
                    strategy_->on_toxicity_update(u);
                };
            }
            frags += bus_.tox->poll();
        }

        if (bus_.dashboard_ctrl) {
            if (!bus_.dashboard_ctrl->on_command) {
                bus_.dashboard_ctrl->on_command = [this](uint8_t cmd) {
                    if (cmd == 0x00 && !trading_halted_) {
                        trading_halted_ = true;
                        metrics_.trading_halted->Set(1.0);
                        bpt::common::log::warn("TRADING HALTED via dashboard kill-switch");
                    } else if (cmd == 0x01 && trading_halted_) {
                        trading_halted_ = false;
                        metrics_.trading_halted->Set(0.0);
                        bpt::common::log::info("Trading RESUMED via dashboard");
                    }
                };
            }
            frags += bus_.dashboard_ctrl->poll();
        }

        // Refdata watchdog runs unconditionally — it must fire during
        // the WaitRefdata startup phase so we don't hang forever if
        // bpt-refdata never comes up.
        check_refdata_watchdog();

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

            if (bus_.portfolio_snap && bus_.portfolio_snap->is_active()) {
                const auto now_ns = bpt::common::util::TscClock::now_epoch_ns();
                bus_.portfolio_snap->publish_if_due(strategy_->get_portfolio_state(), now_ns);

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
                        bus_.portfolio_snap->offer_raw(buf, static_cast<int32_t>(state_json.size()));
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

    if (bus_.md && last_md_hb_recv_ns_ != 0) {
        const uint64_t age_ns = now_ns - last_md_hb_recv_ns_;
        if (age_ns > threshold_ns) {
            bpt::common::log::warn("MD Gateway heartbeat stale ({:.1f}s, threshold={:.1f}s) — pausing trading",
                           age_ns / 1e9,
                           threshold_ns / 1e9);
            stale = true;
        }
    }

    if (bus_.order_gw && last_gw_hb_recv_ns_ != 0) {
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

void StrategyApp::check_refdata_watchdog() {
    // Refdata's heartbeat lives on a 5s cadence
    // (RefdataDeltaPublisher::publish_heartbeat) and its failure mode
    // is silent fee_cache staleness, not lost MD/exec flow — separate
    // concern from md/order-gw heartbeats. Runs every iteration
    // (rate-limited to 1 Hz internally) regardless of startup_gate
    // state, because the StartupTimedOut case needs to fire during
    // the WaitRefdata phase to break the previous hang-forever
    // behaviour when bpt-refdata never comes up.
    //
    // TscClock (wall-epoch ns) — must match the clock used by
    // bpt-refdata's heartbeat publisher (TscClock::now_epoch_ns).
    const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

    if (now_ns - last_refdata_check_ns_ < 1'000'000'000ULL)
        return;
    last_refdata_check_ns_ = now_ns;

    using strategy::RefdataStaleGate;
    const uint64_t hb = bus_.refdata->last_heartbeat_ns();
    const auto state = refdata_stale_gate_.evaluate(now_ns, hb);
    switch (state) {
        case RefdataStaleGate::State::StartupTimedOut: {
            const double elapsed_s = (now_ns - startup_anchor_ns_) / 1e9;
            bpt::common::log::critical(
                "Refdata never published a heartbeat in {:.0f}s — refusing to trade. "
                "Is bpt-refdata running? Stopping process.",
                elapsed_s);
            bpt::common::signal::stop();
            break;
        }
        case RefdataStaleGate::State::GoneStale: {
            // Gate returns GoneStale on every tick during the stale episode
            // (per its contract); edge-gate the WARN to keep logs readable.
            // Metric set + strategy hook are idempotent.
            if (!refdata_stale_logged_) {
                const double age_s = (now_ns - hb) / 1e9;
                bpt::common::log::warn(
                    "Refdata heartbeat stale ({:.1f}s, threshold={:.1f}s) — pausing strategy quotes",
                    age_s,
                    cfg_.strat.strategy.schedule.refdata_heartbeat_timeout_ns / 1e9);
                refdata_stale_logged_ = true;
            }
            metrics_.refdata_stale->Set(1.0);
            strategy_->on_refdata_stale_changed(true);
            break;
        }
        case RefdataStaleGate::State::Recovered: {
            bpt::common::log::info("Refdata heartbeat recovered — resuming strategy quotes");
            metrics_.refdata_stale->Set(0.0);
            strategy_->on_refdata_stale_changed(false);
            refdata_stale_logged_ = false;
            break;
        }
        case RefdataStaleGate::State::Ok:
            break;
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
