#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bpt::strategy::strategy {

// Rolling window realized volatility estimator using log-returns.
// Incrementally maintains mean and variance for O(1) updates.
// Annualizes using a configurable seconds-per-year factor.
class RealizedVolEstimator {
public:
    // window_size: number of log-return samples in the rolling window.
    // sample_interval_ns: expected time between samples (for annualization).
    explicit RealizedVolEstimator(size_t window_size, uint64_t sample_interval_ns);

    // Feed a new mid-price observation. Returns true if enough samples
    // have accumulated for a valid RV estimate.
    bool update(double mid_price, uint64_t timestamp_ns);

    // Annualized realized volatility. Only valid after update() returns true.
    [[nodiscard]] double realized_vol() const;

    // Number of log-return samples currently in the window.
    [[nodiscard]] size_t count() const { return count_; }

    // Whether enough samples have accumulated.
    [[nodiscard]] bool ready() const { return count_ >= min_samples_; }

    void reset();

private:
    size_t window_size_;
    size_t min_samples_;  // require at least half the window
    uint64_t sample_interval_ns_;
    double annualization_factor_;  // sqrt(samples_per_year)

    std::vector<double> returns_;  // circular buffer of log-returns
    size_t head_{0};
    size_t count_{0};

    double sum_{0.0};
    double sum_sq_{0.0};

    double last_price_{0.0};
    uint64_t last_sample_ns_{0};
};

}  // namespace bpt::strategy::strategy
