#pragma once

#include <cstddef>
#include <cstdint>

namespace bpt::features {

// Time-weighted EWMA primitive. Decay is exp(-dt_s / halflife_s), so the
// decay rate is proportional to elapsed wall time, not tick count —
// time-consistent regardless of update cadence.
//
// Stateless wrt the observation source — caller supplies (obs, dt_s)
// each tick. The Ewma* classes below wrap this with the specific
// observation each feature needs.
class TimeWeightedEwma {
public:
    explicit TimeWeightedEwma(double halflife_s) : halflife_s_(halflife_s) {}

    void update(double obs, double dt_s);
    void reset();

    // Restore from a saved value/count — used by warm-start.
    void restore(double value, std::size_t count) {
        value_ = value;
        count_ = count;
    }

    [[nodiscard]] double value() const { return value_; }
    [[nodiscard]] std::size_t count() const { return count_; }
    [[nodiscard]] double halflife_s() const { return halflife_s_; }

private:
    double halflife_s_;
    double value_{0.0};
    std::size_t count_{0};
};

// EWMA of squared time-normalised log returns — per-second variance σ².
// Observation each tick: (log(mid/last_mid) / sqrt(dt_s))²
// Same definition AS uses for its volatility estimator.
class EwmaVariance {
public:
    struct Snapshot {
        double value;
        std::size_t count;
        double last_mid;
        uint64_t last_ns;
    };

    explicit EwmaVariance(double halflife_s) : ewma_(halflife_s) {}

    void update(double mid, uint64_t ts_ns);
    void reset();

    [[nodiscard]] double value() const { return ewma_.value(); }
    [[nodiscard]] std::size_t count() const { return ewma_.count(); }
    [[nodiscard]] double last_mid() const { return last_mid_; }
    [[nodiscard]] uint64_t last_ns() const { return last_ns_; }

    [[nodiscard]] Snapshot snapshot() const { return {ewma_.value(), ewma_.count(), last_mid_, last_ns_}; }
    void restore(const Snapshot& s);

private:
    TimeWeightedEwma ewma_;
    double last_mid_{0.0};
    uint64_t last_ns_{0};
};

// EWMA of signed time-normalised log returns — per-√second drift µ.
class EwmaDrift {
public:
    struct Snapshot {
        double value;
        std::size_t count;
        double last_mid;
        uint64_t last_ns;
    };

    explicit EwmaDrift(double halflife_s) : ewma_(halflife_s) {}

    void update(double mid, uint64_t ts_ns);
    void reset();

    [[nodiscard]] double value() const { return ewma_.value(); }
    [[nodiscard]] std::size_t count() const { return ewma_.count(); }
    [[nodiscard]] double last_mid() const { return last_mid_; }
    [[nodiscard]] uint64_t last_ns() const { return last_ns_; }

    [[nodiscard]] Snapshot snapshot() const { return {ewma_.value(), ewma_.count(), last_mid_, last_ns_}; }
    void restore(const Snapshot& s);

private:
    TimeWeightedEwma ewma_;
    double last_mid_{0.0};
    uint64_t last_ns_{0};
};

// EWMA of per-side trade-arrival rate κ (trades/s).
// Observation each trade: 0.5 / dt_s — the 0.5 splits total trade
// flow across bid and ask sides (assumes symmetric market). Same
// definition AS uses for its κ estimator.
class KappaEstimator {
public:
    struct Snapshot {
        double value;
        std::size_t count;
        uint64_t last_trade_ns;
    };

    explicit KappaEstimator(double halflife_s) : ewma_(halflife_s) {}

    void update(uint64_t trade_ts_ns);
    void reset();

    [[nodiscard]] double value() const { return ewma_.value(); }
    [[nodiscard]] std::size_t count() const { return ewma_.count(); }
    [[nodiscard]] uint64_t last_trade_ns() const { return last_trade_ns_; }

    [[nodiscard]] Snapshot snapshot() const { return {ewma_.value(), ewma_.count(), last_trade_ns_}; }
    void restore(const Snapshot& s);

private:
    TimeWeightedEwma ewma_;
    uint64_t last_trade_ns_{0};
};

}  // namespace bpt::features
