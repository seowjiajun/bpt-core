#include "strategy/strategy/hmm_filter.h"

#include <algorithm>
#include <cmath>

namespace bpt::strategy::strategy {

HmmFilter::Params HmmFilter::default_params() {
    Params p{};

    // Transition matrix — crypto regimes are sticky (self-transition ≈ 0.90).
    // A[i][j] = P(next_state=j | current_state=i).  Rows sum to 1.
    p.A[0] = {0.90, 0.03, 0.04, 0.03};  // TRENDING_UP
    p.A[1] = {0.03, 0.90, 0.04, 0.03};  // TRENDING_DOWN
    p.A[2] = {0.05, 0.05, 0.85, 0.05};  // MEAN_REVERT (slightly less sticky)
    p.A[3] = {0.05, 0.05, 0.05, 0.85};  // HIGH_VOL

    // Emission means per state.
    // Features: [log_return_1min, ewma_vol, spread_bps, book_imbalance, vol_zscore]
    //
    // ewma_vol is the raw std dev of 1s bar log returns.
    // For BTC at 80% ann. vol: σ_1s ≈ 80% / sqrt(365×24×3600) ≈ 0.000142.
    // Quiet (MR) regime at ~60% ann. vol ≈ 0.000106; high-vol ≈ 0.000250+.
    p.mu[0] = {0.0008, 0.000142, 2.0, 0.62, 0.8};   // TRENDING_UP
    p.mu[1] = {-0.0008, 0.000142, 2.0, 0.38, 0.8};  // TRENDING_DOWN
    p.mu[2] = {0.0000, 0.000090, 1.2, 0.50, 0.2};   // MEAN_REVERT
    p.mu[3] = {0.0000, 0.000280, 6.0, 0.50, 1.8};   // HIGH_VOL

    // Emission std devs per state.
    p.sigma[0] = {0.0004, 0.000035, 1.0, 0.12, 0.5};
    p.sigma[1] = {0.0004, 0.000035, 1.0, 0.12, 0.5};
    p.sigma[2] = {0.0003, 0.000020, 0.6, 0.08, 0.4};
    p.sigma[3] = {0.0010, 0.000080, 2.5, 0.15, 0.8};

    // Initial distribution: prior towards mean-revert (most common crypto regime).
    p.pi = {0.15, 0.15, 0.55, 0.15};

    return p;
}

HmmFilter::HmmFilter(Params params) : params_(std::move(params)) {}

double HmmFilter::emission_prob(int k, const std::array<double, D>& obs) const {
    // Log-space product of independent Gaussians (diagonal covariance).
    // Avoids underflow for high-dimensional observations.
    static constexpr double kMinProb = 1e-300;
    double log_p = 0.0;
    for (int d = 0; d < D; ++d) {
        if (params_.sigma[k][d] <= 0.0)
            continue;
        const double z = (obs[d] - params_.mu[k][d]) / params_.sigma[k][d];
        log_p -= 0.5 * z * z + std::log(params_.sigma[k][d]);
    }
    return std::max(std::exp(log_p), kMinProb);
}

void HmmFilter::update(const std::array<double, D>& obs) {
    std::array<double, K> new_alpha{};

    if (!initialized_) {
        // First step: α_0[k] = π[k] × B_k(obs).
        for (int k = 0; k < K; ++k)
            new_alpha[k] = params_.pi[k] * emission_prob(k, obs);
        initialized_ = true;
    } else {
        // Forward step: α_t[k] = B_k(obs) × Σ_j α_{t-1}[j] × A[j][k].
        for (int k = 0; k < K; ++k) {
            double predict = 0.0;
            for (int j = 0; j < K; ++j)
                predict += alpha_[j] * params_.A[j][k];
            new_alpha[k] = emission_prob(k, obs) * predict;
        }
    }

    // Normalise to prevent cumulative underflow.
    double sum = 0.0;
    for (double v : new_alpha)
        sum += v;
    if (sum > 0.0)
        for (double& v : new_alpha)
            v /= sum;
    else
        new_alpha.fill(1.0 / K);  // degenerate: reset to uniform

    alpha_ = new_alpha;
}

HmmFilter::State HmmFilter::dominant() const {
    int best = 0;
    for (int k = 1; k < K; ++k)
        if (alpha_[k] > alpha_[best])
            best = k;
    return static_cast<State>(best);
}

double HmmFilter::confidence() const {
    return alpha_[static_cast<int>(dominant())];
}

const char* HmmFilter::state_name(State s) {
    switch (s) {
        case State::TRENDING_UP:
            return "TRENDING_UP";
        case State::TRENDING_DOWN:
            return "TRENDING_DOWN";
        case State::MEAN_REVERT:
            return "MEAN_REVERT";
        case State::HIGH_VOL:
            return "HIGH_VOL";
        default:
            return "UNKNOWN";
    }
}

}  // namespace bpt::strategy::strategy
