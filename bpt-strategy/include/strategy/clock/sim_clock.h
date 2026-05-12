#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace bpt::strategy::clock {

// Process-wide simulation clock. In backtest mode, the BacktestControl loop
// calls set_now_ns(sim_ts) on every tick; strategy code reads via now_ns()
// instead of system_clock::now() / steady_clock::now() so that successive
// runs over the same tape produce byte-identical outputs.
//
// Live mode never calls set_now_ns and now_ns() falls through to the wall
// clock — no harness change required for production.
//
// Single-threaded backtest path => no contention; relaxed atomics keep the
// hot-path read at one uncontended load.
class SimClock {
public:
    static void set_now_ns(uint64_t ns) noexcept {
        sim_now_ns_.store(ns, std::memory_order_relaxed);
        active_.store(true, std::memory_order_relaxed);
    }

    static uint64_t now_ns() noexcept {
        if (active_.load(std::memory_order_relaxed))
            return sim_now_ns_.load(std::memory_order_relaxed);
        return wall_now_ns();
    }

    static bool active() noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    // Reset to wall-clock fallback. Tests use this to isolate state across
    // cases; production code should never call it.
    static void reset() noexcept {
        active_.store(false, std::memory_order_relaxed);
        sim_now_ns_.store(0, std::memory_order_relaxed);
    }

private:
    static uint64_t wall_now_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    inline static std::atomic<uint64_t> sim_now_ns_{0};
    inline static std::atomic<bool> active_{false};
};

}  // namespace bpt::strategy::clock
