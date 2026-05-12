#pragma once

// Exchange-reject-rate circuit breaker.
//
// Trips when the fraction of exchange REJECTED exec reports over the
// last `window_ns` exceeds `threshold_pct`, provided at least
// `min_events` events landed in that window. Tripping latches: no auto
// clear. The latch is paired with RiskChecker::set_trading_enabled(false)
// the same way the daily-loss kill switch in OrderProcessor is — an
// operator has to restart to clear it, so the underlying cause gets a
// human look before we keep hammering the venue.
//
// Intended signal: "exchange is refusing everything we send" —
// credentials expired, account geo-blocked, margin mode flipped, etc.
// Pre-trade risk rejects (our own RiskChecker) never reach on_exec_event
// so they do NOT pollute this rate. Only adapter-emitted REJECTED events
// count (EXCHANGE_ERROR / exchange-side rejections, plus the no-match
// outcome from the HL phantom-fill reconciler).

#include <cstdint>
#include <deque>

namespace bpt::order_gateway::risk {

class RejectRateBreaker {
public:
    struct Config {
        uint64_t window_ns = 60ULL * 1'000'000'000ULL;  // 60 s
        double threshold_pct = 20.0;                    // trip at >20 %
        uint32_t min_events = 10;                       // ignore tiny samples
        bool enabled = false;                           // default off
    };

    explicit RejectRateBreaker(Config cfg);

    // Register one exchange exec event. `is_reject` must reflect the
    // authoritative ExecStatus::REJECTED, not pre-trade risk rejects.
    void record(bool is_reject, uint64_t now_ns);

    // True once the threshold has been exceeded at least once. Latches —
    // future calls to record() keep returning true even as the window
    // drains, to preserve the "restart required" semantics.
    [[nodiscard]] bool tripped() const noexcept { return tripped_; }

    // Test observability. Not thread-safe; single-threaded usage (same
    // thread that calls record).
    [[nodiscard]] uint32_t total_in_window() const noexcept { return total_; }
    [[nodiscard]] uint32_t rejects_in_window() const noexcept { return rejects_; }

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    Config cfg_;
    // Each entry is (timestamp_ns, is_reject). Head is oldest; evicted
    // lazily at the start of every record() call.
    std::deque<std::pair<uint64_t, bool>> events_;
    uint32_t total_{0};
    uint32_t rejects_{0};
    bool tripped_{false};
};

}  // namespace bpt::order_gateway::risk
