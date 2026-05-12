#pragma once

#include <cstdint>
#include <deque>
#include <utility>

namespace bpt::strategy::strategy {

// Short-horizon realized-volatility gate for trading halts.
//
// Tracks a rolling window of (timestamp_ns, mid_price) pairs and, on
// each update, computes the max |price move| (in bps of current mid)
// observed within the window. If that exceeds `max_bps_per_window`,
// the gate enters a halted state for `halt_duration_ns`. While halted,
// `is_halted()` returns true and callers are expected to skip placing
// new entries (taker strategies) or cancel and refuse to re-quote
// (maker strategies).
//
// Design notes:
//  - Window + halt cooldown are independent: trip on a 1s window,
//    halt for 5s. The 5s lets the move settle before trading resumes.
//  - Rolling max is computed by evicting deque front entries older
//    than `window_ns` on every update. O(amortized 1) per tick.
//  - Measurement is in bps of CURRENT mid, so a 10 bps threshold at
//    $74k BTC means a ~$74 price swing within the window.
//  - No warmup: single-entry windows return max=0 and never trip, so
//    there's no "wait N ticks to activate" gotcha. The first real move
//    in the first window can still trip if it's large enough.
//  - Stateless re-arm: once halt expires, the gate is clean — the
//    history deque continues to roll, and a fresh spike is needed to
//    trip again.
//  - Not thread-safe: called from the strategy thread only.
//
// Header-only for inlining into strategy on_bbo / on_order_book hot
// paths; no .cpp file.
class VolatilityGate {
public:
    struct Config {
        double max_bps_per_window{0.0};            // 0 disables the gate entirely
        uint64_t window_ns{1'000'000'000};         // 1s default
        uint64_t halt_duration_ns{5'000'000'000};  // 5s default
    };

    VolatilityGate() = default;
    explicit VolatilityGate(Config cfg) : cfg_(cfg) {}

    // Adjust the trip threshold at runtime. Exposed so AS can make
    // vol_gate adaptive to observed σ — threshold moves with the vol
    // regime instead of requiring a per-asset tuning pass. No effect
    // on an in-progress halt (halt_end_ns_ unchanged); only future
    // update_and_check() calls see the new value.
    void set_max_bps_per_window(double max_bps) noexcept { cfg_.max_bps_per_window = max_bps; }
    [[nodiscard]] double max_bps_per_window() const noexcept { return cfg_.max_bps_per_window; }

    // Feed a new (timestamp, mid) sample. Returns the new halt state
    // AFTER any trip decision. Call this on every BBO tick even when
    // you don't intend to check halt state — it maintains the rolling
    // window.
    bool update_and_check(double mid, uint64_t now_ns) {
        if (cfg_.max_bps_per_window <= 0.0 || mid <= 0.0)
            return false;  // disabled

        // Evict samples older than window
        while (!history_.empty() && now_ns - history_.front().first > cfg_.window_ns)
            history_.pop_front();

        // Compute max |delta bps| against the current mid BEFORE pushing
        // this sample. Uses current mid as the bps denominator — a trip
        // is "price moved this much relative to where it is now."
        double max_bps = 0.0;
        for (const auto& [ts, px] : history_) {
            const double bps = (px - mid) / mid * 1e4;
            const double abs_bps = bps < 0 ? -bps : bps;
            if (abs_bps > max_bps)
                max_bps = abs_bps;
        }

        history_.emplace_back(now_ns, mid);

        // Trip if above threshold
        if (max_bps > cfg_.max_bps_per_window) {
            halt_end_ns_ = now_ns + cfg_.halt_duration_ns;
            ++trips_;
            last_trip_bps_ = max_bps;
        }

        return now_ns < halt_end_ns_;
    }

    // Returns halt state without updating the window. Useful for
    // callers that want to check status between ticks.
    bool is_halted(uint64_t now_ns) const { return cfg_.max_bps_per_window > 0.0 && now_ns < halt_end_ns_; }

    // Diagnostics
    uint64_t halt_end_ns() const { return halt_end_ns_; }
    uint64_t trips_total() const { return trips_; }
    double last_trip_bps() const { return last_trip_bps_; }
    bool enabled() const { return cfg_.max_bps_per_window > 0.0; }

private:
    Config cfg_{};
    std::deque<std::pair<uint64_t, double>> history_;
    uint64_t halt_end_ns_{0};
    uint64_t trips_{0};
    double last_trip_bps_{0.0};
};

}  // namespace bpt::strategy::strategy
