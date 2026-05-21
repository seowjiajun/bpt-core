#pragma once

// Rolling multi-level Order-Flow Imbalance estimator.
//
// Implements the Cont-Kukanov-Stoikov OFI signal: for each book update,
// compute per-level signed contributions from queue additions, queue
// removals, and price moves, sum them with 1/(k+1) depth weights, and
// sum those contributions over a rolling time window. Normalise by the
// recent average top-K queue depth to get a unitless, scale-free value
// that is comparable across instruments.
//
// Positive OFI = net buy pressure (bid size growing / ask size falling
// / best bid rising / best ask rising). Negative = net sell pressure.
//
// The calculator is self-contained per-instrument: feed it successive
// book snapshots, read out the current rolling value. Zero coupling to
// SBE types — the strategy layer converts MdOrderBook groups into
// plain (price, qty) vectors before calling update().

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace bpt::features {

class OFICalculator {
public:
    struct Config {
        // Max book levels to aggregate. Levels beyond what the snapshot
        // contains are simply ignored.
        int max_levels{5};

        // Rolling window for the OFI sum. 1 second is a reasonable default
        // for crypto perps — OFI signal decay is faster than most other
        // microstructure signals.
        uint64_t window_ns{1'000'000'000ULL};
    };

    using Level = std::pair<double, double>;  // (price, qty)

    explicit OFICalculator(Config cfg);

    // Feed a new book snapshot. Returns the current normalized OFI value
    // over the rolling window — positive = buy pressure, negative = sell
    // pressure. Returns 0 on the very first call (no previous snapshot).
    // Trailing window samples older than `window_ns` are evicted before
    // the fresh contribution is added.
    double update(const std::vector<Level>& bids, const std::vector<Level>& asks, uint64_t timestamp_ns);

    // True once at least one update() has produced a value with a previous
    // snapshot to compare against.
    [[nodiscard]] bool is_warm() const { return warm_; }

    // Last value returned by update(). 0 if never called.
    [[nodiscard]] double value() const { return last_value_; }

    // Rolling average of top-K depth over the window — exposed for
    // diagnostics / metrics.
    [[nodiscard]] double avg_depth() const;

    // Reset all state. Used on reconnect.
    void reset();

private:
    Config cfg_;
    std::vector<Level> prev_bids_;
    std::vector<Level> prev_asks_;

    struct Sample {
        uint64_t ts_ns;
        double contribution;  // level-weighted OFI delta for this event
        double depth;         // top-K total (bid+ask) size at this event
    };
    std::deque<Sample> window_;
    double running_contrib_{0.0};
    double running_depth_{0.0};
    double last_value_{0.0};
    bool warm_{false};
};

}  // namespace bpt::features
