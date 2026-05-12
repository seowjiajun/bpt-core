#pragma once

// Per-adapter disconnect-rate circuit breaker.
//
// Counts exchange disconnect events in a rolling window. Trips (and
// latches — no auto-clear) when at least `threshold` disconnects land
// within `window_ns`. Tripping marks the adapter halted so that
// OrderProcessor refuses further NewOrders to that venue with
// EXCHANGE_ERROR — same reject surface as "adapter not connected".
//
// Intended signal: an exchange adapter that is reconnecting in a tight
// loop (persistent auth failure, geo-block, margin-mode mismatch, TLS
// handshake error). Without this latch we burn rate-limit budget and
// spam error logs until an operator notices.
//
// Different from RejectRateBreaker (ratio over N events). Here every
// record() is a disconnect event; only the *rate* matters, not a ratio.
// Kept deliberately small — no dependency on adapter internals.
//
// Thread-safety: record() and tripped() are both called from the
// adapter IO thread (connect_and_run() runs on one thread, halted() is
// read from the main poll thread). Count is atomic-monotonic via the
// latch; only record() mutates the deque.

#include <atomic>
#include <cstdint>
#include <deque>

namespace bpt::order_gateway::risk {

class DisconnectRateBreaker {
public:
    struct Config {
        uint64_t window_ns = 60ULL * 1'000'000'000ULL;  // 60 s
        uint32_t threshold = 5;                         // 5 disconnects / window
        bool enabled = false;                           // default off
    };

    explicit DisconnectRateBreaker(Config cfg);

    // Replace config and clear all state. Used by OrderAdapterBase
    // to apply the operator-supplied config before start() — the
    // atomic_bool inside the class is not move-assignable, so we
    // can't just `breaker = DisconnectRateBreaker(cfg)`.
    void reset(Config cfg);

    // Register one disconnect event (adapter's connect_and_run()
    // returned or threw). Lazy eviction — old events fall out as new
    // ones land.
    void record(uint64_t now_ns);

    // Latched: returns true once the threshold has been crossed, and
    // stays true for the lifetime of the object. Safe to call from
    // any thread (loads an atomic).
    [[nodiscard]] bool tripped() const noexcept { return tripped_.load(std::memory_order_acquire); }

    // Test observability.
    [[nodiscard]] uint32_t count_in_window() const noexcept { return static_cast<uint32_t>(events_.size()); }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    Config cfg_;
    std::deque<uint64_t> events_;
    std::atomic<bool> tripped_{false};
};

}  // namespace bpt::order_gateway::risk
