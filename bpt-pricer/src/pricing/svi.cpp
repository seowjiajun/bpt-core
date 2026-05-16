#include "pricer/pricing/svi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bpt::pricer::pricing {

namespace {

// Parameter bounds enforced after each Gauss-Newton step. b and s are
// strictly positive; rho lives in (−1, 1) — we clip slightly inside the
// open boundary so the gradient evaluation doesn't degenerate. a has no
// hard bound but the joint a + b·s·sqrt(1−ρ²) ≥ 0 constraint is checked
// post-step.
constexpr double kBMin = 1e-8;
constexpr double kSMin = 1e-6;
constexpr double kRhoClip = 0.999;

inline double clip(double v, double lo, double hi) {
    return std::max(lo, std::min(v, hi));
}

void enforce_bounds(SviParams& p) {
    if (p.b < kBMin) p.b = kBMin;
    if (p.s < kSMin) p.s = kSMin;
    p.rho = clip(p.rho, -kRhoClip, kRhoClip);
    // Joint no-arb floor: a + b·s·sqrt(1−ρ²) must be ≥ 0. If a drifts too
    // negative, lift it to the floor — preserves the wing shape and only
    // adjusts the vertical level.
    const double floor_a = -p.b * p.s * std::sqrt(1.0 - p.rho * p.rho);
    if (p.a < floor_a) p.a = floor_a;
}

// Analytic partial derivatives of w(k) = a + b·(ρ·(k−m) + sqrt((k−m)² + s²))
// wrt each parameter, evaluated at point k. Returns {∂w/∂a, ∂w/∂b, ∂w/∂ρ,
// ∂w/∂m, ∂w/∂s}.
std::array<double, 5> grad_w(double k, const SviParams& p) noexcept {
    const double km = k - p.m;
    const double r = std::sqrt(km * km + p.s * p.s);
    std::array<double, 5> g{};
    g[0] = 1.0;                          // ∂w/∂a
    g[1] = p.rho * km + r;               // ∂w/∂b
    g[2] = p.b * km;                     // ∂w/∂ρ
    // ∂w/∂m = b·(−ρ − (k−m)/r)
    g[3] = p.b * (-p.rho - km / r);
    // ∂w/∂s = b · s / r
    g[4] = p.b * p.s / r;
    return g;
}

// Initial-guess heuristic from observed points. Centered ATM-ish, modest
// wings, neutral skew — good enough for SP500/BTC-style smiles.
SviParams initial_guess(const std::vector<SviFitInput>& points) {
    double min_var = std::numeric_limits<double>::infinity();
    double k_at_min = 0.0;
    for (const auto& p : points) {
        if (p.total_var < min_var) {
            min_var = p.total_var;
            k_at_min = p.k;
        }
    }
    SviParams g;
    g.a = std::max(0.0, min_var * 0.5);  // slightly below observed min
    g.b = 0.1;
    g.rho = -0.3;                        // typical negative skew
    g.m = k_at_min;
    g.s = 0.1;
    enforce_bounds(g);
    return g;
}

// Solve the 5x5 symmetric positive-(semi)definite normal-equations system
// JᵀJ · dθ = Jᵀr. Uses Cholesky factorisation with a small ridge to keep
// the matrix invertible when columns of J are near-colinear (common when
// observed points cluster around ATM). Returns the parameter update vector
// or std::nullopt if the system is unsolvable.
//
// jt_j is column-major flattened 5x5; jt_r is length 5.
std::optional<std::array<double, 5>> solve_5x5(const std::array<double, 25>& jt_j,
                                               const std::array<double, 5>& jt_r) {
    constexpr int N = 5;
    constexpr double kRidge = 1e-10;
    std::array<double, 25> A = jt_j;
    for (int i = 0; i < N; ++i)
        A[i * N + i] += kRidge;

    // Cholesky factorise: A = L · Lᵀ (in-place, lower triangle of A holds L)
    for (int j = 0; j < N; ++j) {
        double diag = A[j * N + j];
        for (int k = 0; k < j; ++k)
            diag -= A[j * N + k] * A[j * N + k];
        if (diag <= 0.0)
            return std::nullopt;
        A[j * N + j] = std::sqrt(diag);
        for (int i = j + 1; i < N; ++i) {
            double s = A[i * N + j];
            for (int k = 0; k < j; ++k)
                s -= A[i * N + k] * A[j * N + k];
            A[i * N + j] = s / A[j * N + j];
        }
    }

    // Forward solve L · y = jt_r
    std::array<double, 5> y{};
    for (int i = 0; i < N; ++i) {
        double s = jt_r[i];
        for (int k = 0; k < i; ++k)
            s -= A[i * N + k] * y[k];
        y[i] = s / A[i * N + i];
    }

    // Back solve Lᵀ · x = y
    std::array<double, 5> x{};
    for (int i = N - 1; i >= 0; --i) {
        double s = y[i];
        for (int k = i + 1; k < N; ++k)
            s -= A[k * N + i] * x[k];
        x[i] = s / A[i * N + i];
    }
    return x;
}

}  // namespace

double svi_total_variance(double k, const SviParams& p) noexcept {
    const double km = k - p.m;
    const double r = std::sqrt(km * km + p.s * p.s);
    return p.a + p.b * (p.rho * km + r);
}

double svi_iv(double k, double T, const SviParams& p) noexcept {
    if (T <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    const double w = svi_total_variance(k, p);
    if (w < 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return std::sqrt(w / T);
}

std::optional<SviFitResult> svi_fit(const std::vector<SviFitInput>& points,
                                    std::size_t max_iterations,
                                    double tolerance) {
    if (points.size() < 3)
        return std::nullopt;

    SviParams theta = initial_guess(points);

    SviFitResult result;
    for (std::size_t iter = 0; iter < max_iterations; ++iter) {
        // Build JᵀJ (5x5) and Jᵀr (5x1) from per-point residuals.
        std::array<double, 25> jt_j{};
        std::array<double, 5> jt_r{};
        double rss = 0.0;
        for (const auto& pt : points) {
            const double w_model = svi_total_variance(pt.k, theta);
            const double r = pt.total_var - w_model;
            rss += r * r;
            const auto g = grad_w(pt.k, theta);
            for (int i = 0; i < 5; ++i) {
                jt_r[i] += g[i] * r;
                for (int j = 0; j < 5; ++j)
                    jt_j[i * 5 + j] += g[i] * g[j];
            }
        }

        const auto step = solve_5x5(jt_j, jt_r);
        if (!step)
            break;

        const std::array<double, 5>& d = *step;
        theta.a += d[0];
        theta.b += d[1];
        theta.rho += d[2];
        theta.m += d[3];
        theta.s += d[4];
        enforce_bounds(theta);

        result.iterations = iter + 1;
        const double step_norm = std::sqrt(
            d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3] + d[4] * d[4]);
        if (step_norm < tolerance) {
            result.converged = true;
            result.params = theta;
            result.rms_residual = std::sqrt(rss / static_cast<double>(points.size()));
            return result;
        }
    }

    // Didn't formally converge — return the last iterate as best-effort if
    // the residual is finite. Callers can decide whether to trust it via
    // the `converged` flag and `rms_residual` magnitude.
    result.params = theta;
    double rss = 0.0;
    for (const auto& pt : points) {
        const double r = pt.total_var - svi_total_variance(pt.k, theta);
        rss += r * r;
    }
    result.rms_residual = std::sqrt(rss / static_cast<double>(points.size()));
    return result;
}

}  // namespace bpt::pricer::pricing
