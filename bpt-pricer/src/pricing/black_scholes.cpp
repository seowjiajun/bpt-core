#include "pricer/pricing/black_scholes.h"

#include <cmath>

namespace bpt::pricer::pricing {

// Constants
static constexpr double kInvSqrt2Pi = 0.3989422804014327;  // 1/sqrt(2*pi)

double norm_pdf(double x) {
    return kInvSqrt2Pi * std::exp(-0.5 * x * x);
}

// Abramowitz & Stegun approximation (|error| < 7.5e-8).
double norm_cdf(double x) {
    if (x < -8.0)
        return 0.0;
    if (x > 8.0)
        return 1.0;

    static constexpr double a1 = 0.319381530;
    static constexpr double a2 = -0.356563782;
    static constexpr double a3 = 1.781477937;
    static constexpr double a4 = -1.821255978;
    static constexpr double a5 = 1.330274429;
    static constexpr double p = 0.2316419;

    const double L = std::abs(x);
    const double k = 1.0 / (1.0 + p * L);
    const double k2 = k * k;
    const double k3 = k2 * k;
    const double k4 = k3 * k;
    const double k5 = k4 * k;

    const double cdf = 1.0 - norm_pdf(L) * (a1 * k + a2 * k2 + a3 * k3 + a4 * k4 + a5 * k5);

    return (x >= 0.0) ? cdf : 1.0 - cdf;
}

// Black-76: F is the forward price. Drift lives in F, so d1 carries only the
// ½σ²T variance term — r enters solely through the discount factor. Greeks are
// w.r.t. the forward.
BsResult bs_call(double F, double K, double T, double r, double sigma) {
    if (T <= 0.0 || sigma <= 0.0 || F <= 0.0 || K <= 0.0) {
        return {0.0, 0.0, 0.0, 0.0, 0.0};
    }

    const double sqrt_T = std::sqrt(T);
    const double d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / (sigma * sqrt_T);
    const double d2 = d1 - sigma * sqrt_T;
    const double df = std::exp(-r * T);
    const double nd1 = norm_pdf(d1);

    const double Nd1 = norm_cdf(d1);
    const double Nd2 = norm_cdf(d2);

    const double price = df * (F * Nd1 - K * Nd2);
    const double delta = df * Nd1;
    const double gamma = df * nd1 / (F * sigma * sqrt_T);
    const double vega = df * F * nd1 * sqrt_T;
    const double theta = -(F * df * nd1 * sigma) / (2.0 * sqrt_T) + r * price;

    return {price, delta, gamma, vega, theta};
}

BsResult bs_put(double F, double K, double T, double r, double sigma) {
    if (T <= 0.0 || sigma <= 0.0 || F <= 0.0 || K <= 0.0) {
        return {0.0, 0.0, 0.0, 0.0, 0.0};
    }

    const double sqrt_T = std::sqrt(T);
    const double d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / (sigma * sqrt_T);
    const double d2 = d1 - sigma * sqrt_T;
    const double df = std::exp(-r * T);
    const double nd1 = norm_pdf(d1);

    const double Nmd1 = norm_cdf(-d1);
    const double Nmd2 = norm_cdf(-d2);

    const double price = df * (K * Nmd2 - F * Nmd1);
    const double delta = -df * Nmd1;
    const double gamma = df * nd1 / (F * sigma * sqrt_T);  // Same as call
    const double vega = df * F * nd1 * sqrt_T;             // Same as call
    const double theta = -(F * df * nd1 * sigma) / (2.0 * sqrt_T) + r * price;

    return {price, delta, gamma, vega, theta};
}

}  // namespace bpt::pricer::pricing
