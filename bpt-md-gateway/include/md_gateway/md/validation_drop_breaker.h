#pragma once

/// \file
/// \brief Per-adapter validation-drop circuit breaker.
///
/// Trips when the share of `DROP` validation results over the rolling
/// window exceeds the configured threshold (and at least `min_events`
/// validations have run). On trip, ValidatingPublisher stops forwarding
/// to its inner publisher — downstream consumers see no further data
/// from this adapter rather than suspect data. Latches: no auto-clear,
/// process restart required (mirrors the order-gateway breakers).
///
/// Intended signal: a feed that is consistently emitting bad ticks
/// (schema drift after a venue upgrade, broken timestamp sequence,
/// crossed books from a buggy delta merger). Strategies reading that
/// feed should stop trading on it rather than act on corrupt data.

#include <cstdint>
#include <deque>

namespace bpt::md_gateway::md {

/// \brief Rolling-window drop-rate breaker.
///
/// Not thread-safe. `record()` is called from the same publisher thread
/// that runs validate/forward. `tripped()` returns a plain bool loaded
/// from that same thread; cross-thread readers should mirror through an
/// atomic if one is added later.
class ValidationDropBreaker {
public:
    struct Config {
        uint64_t window_ns = 60ULL * 1'000'000'000ULL;  ///< rolling window (default 60 s)
        double threshold_pct = 30.0;                    ///< trip when drop ratio exceeds this (default 30 %)
        uint32_t min_events = 50;                       ///< don't evaluate until at least this many samples landed
        bool enabled = false;                           ///< default off; opt-in per adapter once thresholds are tuned
    };

    explicit ValidationDropBreaker(Config cfg);

    /// \brief Register one validation outcome.
    /// \param is_drop true iff MdValidator returned DROP for this attempt
    /// \param now_ns  monotonic timestamp used to roll old events out of the window
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
