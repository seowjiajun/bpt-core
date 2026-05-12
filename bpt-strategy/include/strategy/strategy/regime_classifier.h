#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bpt::strategy::strategy {

// RegimeClassifier — binary regime gate for passive market making.
//
// Maintains a rolling window of log-returns sampled at a fixed interval.
// On each call, exposes:
//   - realized_vol_bps_per_min(): per-minute stdev of log-returns in bps
//   - trend_zscore(): |cumulative log return| / (stdev × sqrt(n)). High
//     when the day has a persistent directional drift; low when returns
//     are mean-reverting / choppy.
//   - classify(): {QUIET, TRENDING, CHOPPY} based on the two signals.
//
// Designed for the empirical APE pattern: profitable days have either
// low vol or high trend strength; loss-making days have non-trivial vol
// with near-zero trend (chop). The classifier returns CHOPPY exactly
// when the strategy should pause quoting.
//
// Hysteresis: once classified CHOPPY, the regime stays CHOPPY for at
// least `chop_cooldown_ns` after the last "qualifying" CHOPPY tick, so
// the strategy doesn't flap at the boundary as vol/trend wiggle.
//
// Pure utility — no IStrategy callbacks, no order management. Strategy
// owns one instance per instrument.
class RegimeClassifier {
public:
    enum class Regime { QUIET, TRENDING, CHOPPY };

    struct Config {
        std::size_t window_size = 60;        // # samples in rolling window
        std::uint64_t sample_interval_ns = 1'000'000'000ULL;  // 1 s nominal
        double quiet_vol_bps_per_min = 5.0;  // RV below this → QUIET regardless of trend
        double trend_threshold_z = 1.0;      // |trend_z| ≥ this → TRENDING
        std::uint64_t chop_cooldown_ns = 120'000'000'000ULL;  // 2 min hysteresis
    };

    RegimeClassifier() : RegimeClassifier(Config{}) {}
    explicit RegimeClassifier(Config cfg)
        : cfg_(cfg), returns_(cfg.window_size, 0.0) {}

    // Feed a new mid-price observation. Returns true once the window has
    // enough samples to classify (≥ window_size / 2).
    bool update(double mid_price, std::uint64_t timestamp_ns) {
        if (mid_price <= 0.0)
            return ready();
        // Throttle: only sample once per sample_interval_ns. Lets the caller
        // fire update() on every tick without inflating the sample rate.
        if (last_sample_ns_ != 0 &&
            timestamp_ns - last_sample_ns_ < cfg_.sample_interval_ns) {
            return ready();
        }

        if (last_price_ > 0.0) {
            const double r = std::log(mid_price / last_price_);
            // Pop the oldest from running sums, then write new one.
            sum_     -= returns_[head_];
            sum_sq_  -= returns_[head_] * returns_[head_];
            returns_[head_] = r;
            sum_     += r;
            sum_sq_  += r * r;
            head_ = (head_ + 1) % cfg_.window_size;
            if (count_ < cfg_.window_size) ++count_;
        }
        last_price_ = mid_price;
        last_sample_ns_ = timestamp_ns;
        return ready();
    }

    [[nodiscard]] bool ready() const {
        return count_ >= cfg_.window_size / 2;
    }

    // Per-minute realized vol in bps. Conversion assumes the configured
    // sample_interval_ns matches reality.
    [[nodiscard]] double realized_vol_bps_per_min() const {
        if (count_ < 2)
            return 0.0;
        const double n = static_cast<double>(count_);
        const double mean = sum_ / n;
        const double var  = (sum_sq_ - n * mean * mean) / (n - 1.0);
        if (var <= 0.0)
            return 0.0;
        const double sd_per_sample = std::sqrt(var);
        // Convert to per-minute. samples_per_minute = 60 sec/min × (1e9 ns/s / sample_interval_ns)
        const double samples_per_minute =
            60.0 * 1e9 / static_cast<double>(cfg_.sample_interval_ns);
        const double sd_per_minute = sd_per_sample * std::sqrt(samples_per_minute);
        return sd_per_minute * 1e4;  // fraction → bps
    }

    // Trend z-score: |Σ returns| / (sd_per_sample × √n). Asymptotically a
    // standard normal under the null of zero-mean i.i.d. returns; values
    // ≥ 1.0 suggest a persistent directional component.
    //
    // Edge case: when var ≈ 0 (perfectly linear drift) AND |Σ returns| > 0,
    // the data is maximally trending — return a large finite value rather
    // than 0/0 = nan. Matches the limit interpretation: a deterministic
    // drift has infinite signal-to-noise.
    [[nodiscard]] double trend_zscore() const {
        if (count_ < 2)
            return 0.0;
        const double n = static_cast<double>(count_);
        const double mean = sum_ / n;
        const double var  = (sum_sq_ - n * mean * mean) / (n - 1.0);
        if (var <= 0.0) {
            return std::abs(sum_) > 0.0 ? 1e6 : 0.0;
        }
        const double sd = std::sqrt(var);
        return std::abs(sum_) / (sd * std::sqrt(n));
    }

    // Classify the current regime. CHOPPY is sticky for chop_cooldown_ns
    // so the strategy doesn't flap at the boundary.
    Regime classify(std::uint64_t now_ns) {
        if (!ready())
            return Regime::QUIET;
        const double rv = realized_vol_bps_per_min();
        if (rv < cfg_.quiet_vol_bps_per_min)
            return Regime::QUIET;
        const double tz = trend_zscore();
        Regime raw = (tz >= cfg_.trend_threshold_z) ? Regime::TRENDING : Regime::CHOPPY;

        if (raw == Regime::CHOPPY) {
            last_chop_ns_ = now_ns;
            return Regime::CHOPPY;
        }
        // Hysteresis: stay CHOPPY for cooldown after the last qualifying tick.
        if (last_chop_ns_ != 0 && now_ns - last_chop_ns_ < cfg_.chop_cooldown_ns)
            return Regime::CHOPPY;
        return raw;
    }

    // Reset all state. Called on snapshot replay or when the instrument
    // drops out of view.
    void reset() {
        std::fill(returns_.begin(), returns_.end(), 0.0);
        head_ = count_ = 0;
        sum_ = sum_sq_ = 0.0;
        last_price_ = 0.0;
        last_sample_ns_ = 0;
        last_chop_ns_ = 0;
    }

    [[nodiscard]] const Config& config() const { return cfg_; }

private:
    Config cfg_;
    std::vector<double> returns_;
    std::size_t head_{0};
    std::size_t count_{0};
    double sum_{0.0};
    double sum_sq_{0.0};
    double last_price_{0.0};
    std::uint64_t last_sample_ns_{0};
    std::uint64_t last_chop_ns_{0};
};

inline const char* regime_name(RegimeClassifier::Regime r) {
    switch (r) {
        case RegimeClassifier::Regime::QUIET:    return "QUIET";
        case RegimeClassifier::Regime::TRENDING: return "TRENDING";
        case RegimeClassifier::Regime::CHOPPY:   return "CHOPPY";
    }
    return "?";
}

}  // namespace bpt::strategy::strategy
