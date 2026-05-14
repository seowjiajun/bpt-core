#include "strategy/app/strategy_service.h"

#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <filesystem>

// Shutdown-flatten orchestration. Extracted from strategy_service.cpp so
// the top-level file stays focused on lifecycle; this is self-contained
// enough to read top-to-bottom as a single "here's what the process
// does on SIGTERM" story.

namespace bpt::strategy {

void StrategyService::shutdown_flatten() {
    if (!strategy_) {
        return;
    }
    bpt::common::log::warn("Shutdown flatten starting — cancelling resting orders and closing open positions");

    // Pre-drain: process any exec reports already queued on the gateway
    // BEFORE we ask the strategy to flatten. If a fill happened in the
    // last few ms of the main loop, it's still in flight and the
    // strategy's internal net_qty is stale — it would see net_qty=0 and
    // not fire an unwind, leaving a real exchange-side position open.
    // A short pre-drain lets PositionTracker catch up to reality. Only
    // poll the order gateway — fresh MD ticks would re-invoke strategy
    // handlers and could fire new entries.
    if (bus_.order_gw) {
        const uint64_t pre_drain_start = bpt::common::util::TscClock::now_epoch_ns();
        constexpr uint64_t kPreDrainBudgetNs = 1'000'000'000ULL;
        while (bpt::common::util::TscClock::now_epoch_ns() - pre_drain_start < kPreDrainBudgetNs) {
            const int frags = bus_.order_gw->poll();
            if (frags == 0)
                __builtin_ia32_pause();
        }
    }

    try {
        strategy_->on_shutdown_flatten();
    } catch (const std::exception& e) {
        bpt::common::log::error("on_shutdown_flatten threw: {}", e.what());
    }

    // Post-drain: loop until the strategy says all its
    // cancels/unwinds have reached a terminal status, or until the
    // drain budget expires. Previous implementation sleep-spun the
    // full 5s regardless of whether orders had already ack'd — fine
    // in the happy path, wasteful; and worse, if the exchange was
    // slow (network hiccup, adapter reconnect mid-flatten) the budget
    // could expire with orders still live. We now log clearly when
    // the budget expires so ops know to look.
    //
    // kMinDrainNs enforces a floor so the shutdown-flatten's cancel_all
    // has time to complete its async round-trip (strategy → OG → HL
    // /info openOrders + batch cancel on /exchange → exec reports back
    // to strategy). has_pending_flatten() tracks only tracked orders,
    // so it can return false before cancel_all's orphan-sweep exec
    // reports arrive; without this floor the drain exits prematurely
    // and orphan orders survive into the next session. 2s covers the
    // typical HL round-trip comfortably with headroom for network jitter.
    const uint64_t drain_start_ns = bpt::common::util::TscClock::now_epoch_ns();
    constexpr uint64_t kMinDrainNs = 2ULL * 1'000'000'000ULL;
    constexpr uint64_t kDrainBudgetNs = 5ULL * 1'000'000'000ULL;
    bool drained_cleanly = true;
    if (bus_.order_gw) {
        while (true) {
            const uint64_t elapsed = bpt::common::util::TscClock::now_epoch_ns() - drain_start_ns;
            if (elapsed >= kDrainBudgetNs) {
                if (strategy_->has_pending_flatten()) {
                    bpt::common::log::error(
                        "shutdown drain budget ({} ms) expired with pending "
                        "orders still in flight — process exiting with live exchange-side "
                        "state. Investigate via order-gateway logs and exchange console.",
                        kDrainBudgetNs / 1'000'000ULL);
                    drained_cleanly = false;
                }
                break;
            }
            // Need both: tracked orders drained AND minimum cancel_all
            // settle time elapsed. Tracked orders typically clear in
            // <100ms; cancel_all's orphan sweep takes 200–500ms.
            if (elapsed >= kMinDrainNs && !strategy_->has_pending_flatten()) {
                bpt::common::log::info("shutdown drain completed cleanly in {} ms", elapsed / 1'000'000ULL);
                break;
            }
            const int frags = bus_.order_gw->poll();
            if (frags == 0)
                __builtin_ia32_pause();
        }
    }
    (void)drained_cleanly;  // reserved: may gate exit code in the future

    // Request a fresh AccountSnapshot from every configured exchange so the
    // dashboard HoldingsPanel reflects the post-flatten state (instead of
    // waiting up to the periodic republish interval in order-gateway).
    if (startup_gate_) {
        try {
            startup_gate_->refresh_account_snapshots();
        } catch (const std::exception& e) {
            bpt::common::log::error("refresh_account_snapshots threw: {}", e.what());
        }
    }

    // Brief secondary drain so the refresh snapshot propagates through
    // the bus before we exit.
    if (bus_.order_gw) {
        const uint64_t t1 = bpt::common::util::TscClock::now_epoch_ns();
        constexpr uint64_t kSnapDrainBudgetNs = 1'000'000'000ULL;
        while (bpt::common::util::TscClock::now_epoch_ns() - t1 < kSnapDrainBudgetNs) {
            int frags = bus_.order_gw->poll();
            if (frags == 0)
                __builtin_ia32_pause();
        }
    }

    // Warm-start save: EWMA / regime state depends only on market data
    // (not our positions), so saving is safe even when the drain didn't
    // complete cleanly. The TTL on load is the real safety net against
    // a stale restart.
    const auto& ws = cfg_.strat.strategy.warm_start;
    if (!ws.state_dir.empty()) {
        const auto path = std::filesystem::path(ws.state_dir) / (std::to_string(cfg_.strat.correlation_id) + ".json");
        try {
            strategy_->save_state(path.string());
        } catch (const std::exception& e) {
            bpt::common::log::error("save_state threw: {}", e.what());
        }
    }

    bpt::common::log::warn("Shutdown flatten complete");
}

}  // namespace bpt::strategy
