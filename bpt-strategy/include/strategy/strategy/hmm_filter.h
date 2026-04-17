#pragma once

#include <array>

namespace bpt::strategy::strategy {

// Online Hidden Markov Model regime filter — forward algorithm.
//
// K = 4 hidden states:
//   0 = TRENDING_UP   — sustained upward directional move
//   1 = TRENDING_DOWN — sustained downward directional move
//   2 = MEAN_REVERT   — low-vol, oscillating around a level
//   3 = HIGH_VOL      — elevated vol, wide spreads, risk-off
//
// D = 5 observation features (computed per 1s bar):
//   0 = 1-min rolling log return (sum of last 60 bar log returns)
//   1 = EWMA realised vol (std dev of 1s bar log returns, λ=0.94)
//   2 = bid-ask spread (bps)
//   3 = order-book imbalance  bid_qty / (bid_qty + ask_qty)  ∈ [0, 1]
//   4 = trade-volume Z-score (rolling 60 bars)
//
// Emission model: per-state diagonal Gaussian.
// Parameters default to crypto-perp priors and can be overridden via
// TOML strategy.params at construction time.
class HmmFilter {
public:
    static constexpr int K = 4;
    static constexpr int D = 5;

    enum class State : int {
        TRENDING_UP = 0,
        TRENDING_DOWN = 1,
        MEAN_REVERT = 2,
        HIGH_VOL = 3,
    };

    struct Params {
        std::array<std::array<double, K>, K> A;      // A[i][j] = P(next=j | cur=i)
        std::array<std::array<double, D>, K> mu;     // emission means per state
        std::array<std::array<double, D>, K> sigma;  // emission std devs per state
        std::array<double, K> pi;                    // initial state distribution
    };

    // Default parameters calibrated for liquid crypto perpetual futures
    // (BTC/ETH at ~80% annualised vol, 1s bar sampling).
    static Params default_params();

    explicit HmmFilter(Params params = default_params());

    // Feed a new observation vector.  Initialises α from π on the first call.
    void update(const std::array<double, D>& obs);

    // Normalised posterior: α[k] = P(state=k | obs_1..t)
    [[nodiscard]] const std::array<double, K>& alpha() const { return alpha_; }

    // Argmax state.
    [[nodiscard]] State dominant() const;

    // Probability of the dominant state (confidence indicator).
    [[nodiscard]] double confidence() const;

    // True after the first call to update().
    [[nodiscard]] bool ready() const { return initialized_; }

    static const char* state_name(State s);

private:
    double emission_prob(int k, const std::array<double, D>& obs) const;

    Params params_;
    std::array<double, K> alpha_{};
    bool initialized_{false};
};

}  // namespace bpt::strategy::strategy
