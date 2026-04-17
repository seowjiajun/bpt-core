#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>

namespace bpt::analytics::analysis {

// Tracks per-side fill rate (fills / total outcomes) and time-to-fill
// (ACKED → FILLED latency) from exec report lifecycle events.
//
// Feed it ACKED, FILLED, PARTIAL, and CANCELLED events. It matches
// fills and cancels to their original ack to compute:
//   - fill_rate: what fraction of posted orders actually fill
//   - mean_ttf_ms: average time between ack and fill
//
// A spike in fill rate means someone is sweeping your quotes.
// A drop in time-to-fill means fills are happening faster than
// normal — potential sniper / informed flow.
class FillRateTracker {
public:
    struct Config {
        std::size_t window_size{100};   // rolling window of completed orders
    };

    FillRateTracker() : cfg_{} {}
    explicit FillRateTracker(Config cfg) : cfg_(cfg) {}

    // Record an ACKED order — starts the clock for time-to-fill.
    void on_acked(uint64_t order_id, int side_sign, uint64_t ack_ns);

    // Record a FILLED order — computes time-to-fill and counts as a fill.
    void on_filled(uint64_t order_id, uint64_t fill_ns);

    // Record a CANCELLED order — counts as a miss (no fill).
    void on_cancelled(uint64_t order_id, uint64_t cancel_ns);

    struct SideStats {
        double fill_rate;       // 0.0–1.0, NaN if no data
        double mean_ttf_ms;     // average time-to-fill in milliseconds, NaN if no fills
        uint32_t fills;
        uint32_t cancels;
        uint32_t total;
    };

    // Get stats for a side (+1 = BUY, -1 = SELL).
    [[nodiscard]] SideStats stats(int side_sign) const;

    std::size_t pending_count() const { return pending_.size(); }

private:
    struct PendingOrder {
        int side_sign;
        uint64_t ack_ns;
    };

    struct Outcome {
        int side_sign;
        bool filled;            // true = filled, false = cancelled
        double ttf_ms;          // time-to-fill in ms (0 if cancelled)
    };

    Config cfg_;
    std::unordered_map<uint64_t, PendingOrder> pending_;  // order_id → ack state
    std::deque<Outcome> window_;
};

}  // namespace bpt::analytics::analysis
