#pragma once

/// \file
/// \brief Gatheral SVI smile parameterisation + least-squares calibration.
///
/// Per-expiry slice: total variance w(k) = a + b·(ρ·(k-m) + sqrt((k-m)² + s²))
/// where:
///   k = ln(K/F)  log-moneyness
///   w(k) = σ²(k)·T  total variance at log-moneyness k
///
/// Five parameters per slice:
///   a  - vertical level (min total variance asymptote)
///   b  - wing angle (overall slope, must be ≥ 0)
///   ρ  - rotation parameter ∈ (−1, 1), controls skew
///   m  - horizontal translation (smile minimum location)
///   s  - smile curvature around m (must be > 0)
///
/// MVP calibration:
///   - Gauss-Newton iterations on the residual r_i = w_obs(k_i) − w_model(k_i)
///   - Box constraints enforced via parameter clipping after each step
///   - Parameter-level no-arb guard: a + b·s·sqrt(1−ρ²) ≥ 0
///
/// Follow-ups (not in MVP — file TODOs when relevant):
///   - Full Levenberg-Marquardt damping (more robust convergence)
///   - Butterfly arb constraint (Gatheral's wing inequality)
///   - Calendar arb across expiries (monotone total variance in T)
///   - Multiple random restarts to escape local minima

#include <cstddef>
#include <optional>
#include <vector>

namespace bpt::pricer::pricing {

struct SviParams {
    double a{0.0};    ///< vertical level (min total variance)
    double b{0.1};    ///< wing angle, must be ≥ 0
    double rho{0.0};  ///< rotation, must satisfy |rho| < 1
    double m{0.0};    ///< horizontal translation
    double s{0.1};    ///< smile curvature, must be > 0
};

/// Evaluate total variance w(k) at log-moneyness `k` under the given SVI params.
/// w(k) is always ≥ 0 when params satisfy the arb-free parameter constraints.
double svi_total_variance(double k, const SviParams& p) noexcept;

/// Evaluate IV(k) at time-to-expiry T from total variance: σ(k) = sqrt(w(k) / T).
/// Returns NaN if T ≤ 0 or w(k) < 0.
double svi_iv(double k, double T, const SviParams& p) noexcept;

struct SviFitInput {
    double k;             ///< log-moneyness ln(K/F)
    double total_var;     ///< observed total variance σ²·T
};

struct SviFitResult {
    SviParams params;
    double rms_residual{0.0};   ///< residual RMS after fit (in total-variance units)
    std::size_t iterations{0};  ///< Gauss-Newton steps taken
    bool converged{false};
};

/// Calibrate SVI parameters to observed (k, total_variance) pairs.
/// Returns nullopt if `points.size() < 3` (under-determined) or if the solver
/// fails to converge to a valid parameter set.
///
/// `max_iterations` caps Gauss-Newton steps (default 100).
/// `tolerance` is the param-vector L2 step size below which we declare
/// convergence (default 1e-6).
std::optional<SviFitResult> svi_fit(const std::vector<SviFitInput>& points,
                                    std::size_t max_iterations = 100,
                                    double tolerance = 1e-6);

}  // namespace bpt::pricer::pricing
