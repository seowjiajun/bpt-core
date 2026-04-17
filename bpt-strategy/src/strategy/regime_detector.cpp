#include "strategy/strategy/regime_detector.h"
#include "strategy/strategy/hurst_estimator.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bpt::strategy::strategy {

void RegimeDetector::update(double mid) {
    if (mid <= 0.0)
        return;

    if (last_mid_ > 0.0) {
        const double log_ret = std::log(mid / last_mid_);
        returns_.push_back(log_ret);
        if (returns_.size() > cfg_.hurst_window)
            returns_.pop_front();
    }
    last_mid_ = mid;
    ++tick_count_;

    // Only evaluate Hurst periodically — it's O(N) and changes slowly.
    if (tick_count_ % cfg_.eval_interval != 0)
        return;
    if (returns_.size() < cfg_.warmup_samples)
        return;

    // Copy returns to contiguous buffer for the Hurst estimator.
    std::vector<double> buf(returns_.begin(), returns_.end());
    hurst_ = compute_hurst_multi_window(buf.data(), buf.size(), buf.size());

    regime_ = classify(hurst_);
}

RegimeDetector::Regime RegimeDetector::classify(double hurst) const {
    // Hysteresis to prevent rapid flipping.
    if (regime_ == Regime::MEAN_REVERT) {
        if (hurst > cfg_.trend_threshold)
            return Regime::TRENDING;
        if (hurst > cfg_.mean_revert_threshold + cfg_.hysteresis)
            return Regime::NEUTRAL;
        return Regime::MEAN_REVERT;
    }

    if (regime_ == Regime::TRENDING) {
        if (hurst < cfg_.mean_revert_threshold)
            return Regime::MEAN_REVERT;
        if (hurst < cfg_.trend_threshold - cfg_.hysteresis)
            return Regime::NEUTRAL;
        return Regime::TRENDING;
    }

    // From NEUTRAL or WARMING_UP.
    if (hurst < cfg_.mean_revert_threshold)
        return Regime::MEAN_REVERT;
    if (hurst > cfg_.trend_threshold)
        return Regime::TRENDING;
    return Regime::NEUTRAL;
}

double RegimeDetector::gamma_multiplier() const {
    switch (regime_) {
        case Regime::MEAN_REVERT: return cfg_.gamma_mult_mean_revert;
        case Regime::TRENDING:    return cfg_.gamma_mult_trending;
        case Regime::NEUTRAL:     return cfg_.gamma_mult_neutral;
        case Regime::WARMING_UP:  return cfg_.gamma_mult_neutral;
    }
    return 1.0;
}

const char* RegimeDetector::name(Regime r) {
    switch (r) {
        case Regime::WARMING_UP:  return "WARMING_UP";
        case Regime::MEAN_REVERT: return "MEAN_REVERT";
        case Regime::NEUTRAL:     return "NEUTRAL";
        case Regime::TRENDING:    return "TRENDING";
    }
    return "UNKNOWN";
}

}  // namespace bpt::strategy::strategy
