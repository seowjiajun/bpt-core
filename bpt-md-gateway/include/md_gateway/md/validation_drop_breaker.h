#pragma once

// Per-adapter validation-drop circuit breaker (md-gateway side).
//
// Trips when the share of `DROP` validation results over the rolling
// window `window_ns` exceeds `threshold_pct`, provided at least
// `min_events` validations have run. On trip: ValidatingPublisher stops
// forwarding to the inner publisher — downstream consumers see no
// further data from this adapter rather than suspect data. Latches — no
// auto-clear, mirroring the order-gateway breakers.
//
// Intended signal: a feed that is consistently emitting bad ticks
// (schema drift after a venue upgrade, bad timestamp sequence, crossed
// books from a buggy delta merger). Strategies reading that feed
// should stop trading on it rather than act on corrupt data.
//
// Not thread-safe: record() is called from the same publisher thread
// that drives validate/forward. tripped() returns a plain bool loaded
// by the same thread — callers on other threads reading the gate
// should use the atomic mirror if one is added later.

#include <cstdint>
#include <deque>

namespace bpt::md_gateway::md {

class ValidationDropBreaker {
public:
    struct Config {
        uint64_t window_ns = 60ULL * 1'000'000'000ULL;  // 60 s
        double   threshold_pct = 30.0;                    // trip at >30 %
        uint32_t min_events = 50;                         // ignore tiny samples
        bool     enabled = false;                         // default off
    };

    explicit ValidationDropBreaker(Config cfg);

    // Register one validation outcome. `is_drop` reflects whether the
    // MdValidator returned DROP for this publish attempt.
    void record(bool is_drop, uint64_t now_ns);

    [[nodiscard]] bool tripped() const noexcept { return tripped_; }

    [[nodiscard]] uint32_t total_in_window() const noexcept { return total_; }
    [[nodiscard]] uint32_t drops_in_window() const noexcept { return drops_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    Config cfg_;
    std::deque<std::pair<uint64_t, bool>> events_;
    uint32_t total_{0};
    uint32_t drops_{0};
    bool tripped_{false};
};

}  // namespace bpt::md_gateway::md
