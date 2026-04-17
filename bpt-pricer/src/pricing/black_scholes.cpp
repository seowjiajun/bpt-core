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

BsResult bs_call(double S, double K, double T, double r, double sigma) {
    if (T <= 0.0 || sigma <= 0.0 || S <= 0.0 || K <= 0.0) {
        return {0.0, 0.0, 0.0, 0.0, 0.0};
    }

    const double sqrt_T = std::sqrt(T);
    const double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * sqrt_T);
    const double d2 = d1 - sigma * sqrt_T;
    const double df = std::exp(-r * T);
    const double nd1 = norm_pdf(d1);

    const double Nd1 = norm_cdf(d1);
    const double Nd2 = norm_cdf(d2);

    const double price = S * Nd1 - K * df * Nd2;
    const double delta = Nd1;
    const double gamma = nd1 / (S * sigma * sqrt_T);
    const double vega = S * nd1 * sqrt_T;
    const double theta = -(S * nd1 * sigma) / (2.0 * sqrt_T) - r * K * df * Nd2;

    return {price, delta, gamma, vega, theta};
}

BsResult bs_put(double S, double K, double T, double r, double sigma) {
    if (T <= 0.0 || sigma <= 0.0 || S <= 0.0 || K <= 0.0) {
        return {0.0, 0.0, 0.0, 0.0, 0.0};
    }

    const double sqrt_T = std::sqrt(T);
    const double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * sqrt_T);
    const double d2 = d1 - sigma * sqrt_T;
    const double df = std::exp(-r * T);
    const double nd1 = norm_pdf(d1);

    const double Nmd1 = norm_cdf(-d1);
    const double Nmd2 = norm_cdf(-d2);

    const double price = K * df * Nmd2 - S * Nmd1;
    const double delta = -Nmd1;
    const double gamma = nd1 / (S * sigma * sqrt_T);  // Same as call
    const double vega = S * nd1 * sqrt_T;             // Same as call
    const double theta = -(S * nd1 * sigma) / (2.0 * sqrt_T) + r * K * df * Nmd2;

    return {price, delta, gamma, vega, theta};
}

}  // namespace bpt::pricer::pricing
