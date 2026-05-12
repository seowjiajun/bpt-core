#pragma once

#include <cstdint>
#include <deque>

namespace bpt::analytics::analysis {

// Tracks a single fill's post-fill markout at fixed time horizons.
//
// On each BBO tick, check_horizons() scans pending fills and records
// the mid move at 1s / 5s / 30s after the fill. Once all three
// horizons are logged, the fill is complete and can be consumed by
// the toxicity scorer.
//
// Markout sign convention:
//   markout = side_sign * (mid_at_horizon - mid_at_fill) / mid_at_fill * 1e4
//   Positive = spread capture (mid moved in our direction)
//   Negative = adverse selection (mid moved against us)
class MarkoutTracker {
public:
    struct Config {
        uint64_t horizon_1_ns{1'000'000'000ULL};   // 1s
        uint64_t horizon_2_ns{5'000'000'000ULL};   // 5s
        uint64_t horizon_3_ns{30'000'000'000ULL};  // 30s
        std::size_t max_pending{64};               // cap to prevent unbounded growth
    };

    // A completed markout observation ready for the toxicity scorer.
    struct Observation {
        uint64_t instrument_id;
        uint64_t fill_ts_ns;
        int side_sign;  // +1 for BUY, -1 for SELL
        double fill_price;
        double fill_mid;  // mid at fill time
        double markout_1s_bps;
        double markout_5s_bps;
        double markout_30s_bps;
    };

    MarkoutTracker() : cfg_{} {}
    explicit MarkoutTracker(Config cfg);

    // Record a new fill to track.
    void on_fill(uint64_t instrument_id, int side_sign, double fill_price, double current_mid, uint64_t fill_ts_ns);

    // Feed a BBO tick. Checks all pending fills against the three
    // horizons. Returns the number of newly completed observations
    // (call consume() to retrieve them).
    int on_tick(double mid, uint64_t now_ns);

    // Retrieve and clear completed observations.
    std::deque<Observation> consume();

    std::size_t pending_count() const { return pending_.size(); }

private:
    struct PendingFill {
        uint64_t instrument_id;
        uint64_t fill_ts_ns;
        int side_sign;
        double fill_price;
        double fill_mid;
        double markout_1s_bps{0.0};
        double markout_5s_bps{0.0};
        double markout_30s_bps{0.0};
        bool logged_1s{false};
        bool logged_5s{false};
        bool logged_30s{false};
    };

    double compute_markout_bps(const PendingFill& pf, double mid) const;

    Config cfg_;
    std::deque<PendingFill> pending_;
    std::deque<Observation> completed_;
};

}  // namespace bpt::analytics::analysis
