#include "strategy/strategy/realized_vol_estimator.h"

#include <cmath>

namespace bpt::strategy::strategy {

RealizedVolEstimator::RealizedVolEstimator(size_t window_size, uint64_t sample_interval_ns)
    : window_size_(window_size),
      min_samples_(window_size / 2),
      sample_interval_ns_(sample_interval_ns),
      returns_(window_size, 0.0) {
    // Compute annualization factor: sqrt(number_of_samples_per_year).
    // 365.25 days * 24h * 3600s = 31557600 seconds per year.
    constexpr double kSecondsPerYear = 365.25 * 24.0 * 3600.0;
    const double sample_interval_s = static_cast<double>(sample_interval_ns) / 1e9;
    const double samples_per_year = kSecondsPerYear / sample_interval_s;
    annualization_factor_ = std::sqrt(samples_per_year);
}

bool RealizedVolEstimator::update(double mid_price, uint64_t timestamp_ns) {
    if (mid_price <= 0.0)
        return ready();

    // First observation — just record the price.
    if (last_price_ <= 0.0) {
        last_price_ = mid_price;
        last_sample_ns_ = timestamp_ns;
        return false;
    }

    // Throttle: only sample at the configured interval.
    if (timestamp_ns - last_sample_ns_ < sample_interval_ns_)
        return ready();

    const double log_ret = std::log(mid_price / last_price_);
    last_price_ = mid_price;
    last_sample_ns_ = timestamp_ns;

    // If window is full, remove the oldest sample.
    if (count_ == window_size_) {
        const double old = returns_[head_];
        sum_ -= old;
        sum_sq_ -= old * old;
    } else {
        ++count_;
    }

    returns_[head_] = log_ret;
    sum_ += log_ret;
    sum_sq_ += log_ret * log_ret;
    head_ = (head_ + 1) % window_size_;

    return ready();
}

double RealizedVolEstimator::realized_vol() const {
    if (count_ < 2)
        return 0.0;

    const double n = static_cast<double>(count_);
    const double mean = sum_ / n;
    const double variance = (sum_sq_ / n) - (mean * mean);

    // Annualized: sigma * sqrt(samples_per_year)
    return std::sqrt(std::max(variance, 0.0)) * annualization_factor_;
}

void RealizedVolEstimator::reset() {
    head_ = 0;
    count_ = 0;
    sum_ = 0.0;
    sum_sq_ = 0.0;
    last_price_ = 0.0;
    last_sample_ns_ = 0;
    std::fill(returns_.begin(), returns_.end(), 0.0);
}

}  // namespace bpt::strategy::strategy
