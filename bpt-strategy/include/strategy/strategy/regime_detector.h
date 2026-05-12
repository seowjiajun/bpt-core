#pragma once

// Rolling market regime detector using Hurst exponent + variance ratio.
//
// Classifies short-horizon market microstructure into:
//   MEAN_REVERT  — H < 0.45, spread capture is favorable
//   TRENDING     — H > 0.55, adverse selection risk is high
//   NEUTRAL      — 0.45 ≤ H ≤ 0.55, random walk
//
// Hysteresis prevents rapid flipping between regimes. The detector
// also suggests a gamma multiplier for AS: lower gamma (tighter
// spreads) in mean-reversion, higher gamma (wider spreads) in trending.
//
// Designed as a shared utility — any strategy can instantiate one.
// Fed from BBO ticks (log mid returns). Not thread-safe.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace bpt::strategy::strategy {

class RegimeDetector {
public:
    enum class Regime : uint8_t {
        WARMING_UP,
        MEAN_REVERT,
        NEUTRAL,
        TRENDING,
    };

    struct Config {
        // Hurst thresholds
        double mean_revert_threshold{0.45};
        double trend_threshold{0.55};
        double hysteresis{0.03};

        // Hurst estimation window (in return samples)
        std::size_t hurst_window{200};

        // Minimum samples before classifying
        std::size_t warmup_samples{50};

        // Gamma multipliers per regime (applied to base gamma)
        double gamma_mult_mean_revert{0.6};  // tighter spreads
        double gamma_mult_neutral{1.0};      // unchanged
        double gamma_mult_trending{1.8};     // wider spreads

        // How often to recompute Hurst (every N ticks). Computing
        // Hurst on every tick is wasteful — it changes slowly.
        std::size_t eval_interval{20};
    };

    RegimeDetector() : cfg_{} {}
    explicit RegimeDetector(Config cfg) : cfg_(cfg) {}

    // Feed a new mid price. Call on every BBO tick.
    // Internally computes log returns and periodically re-evaluates
    // the Hurst exponent.
    void update(double mid);

    // Current regime classification.
    [[nodiscard]] Regime regime() const { return regime_; }
    [[nodiscard]] const char* regime_name() const { return name(regime_); }

    // Latest Hurst exponent (0.5 if not yet computed).
    [[nodiscard]] double hurst() const { return hurst_; }

    // Gamma multiplier suggested for the current regime.
    [[nodiscard]] double gamma_multiplier() const;

    // True once enough samples have been collected.
    [[nodiscard]] bool is_warm() const { return regime_ != Regime::WARMING_UP; }

    [[nodiscard]] std::size_t tick_count() const { return tick_count_; }

    static const char* name(Regime r);

    // Opaque snapshot of internal state — consumed by the strategy's
    // warm-start serialisation. Intentionally plain data (no deque /
    // no nlohmann::json dependency) so the serializer owns the wire
    // format and the detector stays small.
    struct StateSnapshot {
        Regime regime{Regime::WARMING_UP};
        double hurst{0.5};
        double last_mid{0.0};
        std::vector<double> returns;
        std::size_t tick_count{0};
    };

    [[nodiscard]] StateSnapshot snapshot_state() const;

    // Restore from a prior snapshot. The Config is NOT overwritten —
    // callers should already have constructed the detector with the
    // intended config; this only repopulates the runtime state.
    // Silently clamps `returns` to cfg_.hurst_window if oversized
    // (handles config shrinking between runs).
    void restore_state(const StateSnapshot& snap);

private:
    Regime classify(double hurst) const;

    Config cfg_;
    Regime regime_{Regime::WARMING_UP};
    double hurst_{0.5};
    double last_mid_{0.0};
    std::deque<double> returns_;
    std::size_t tick_count_{0};
};

}  // namespace bpt::strategy::strategy
