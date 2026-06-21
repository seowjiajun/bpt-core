#include "pricer/pricing/iv_solver.h"

#include "pricer/pricing/black_scholes.h"

#include <algorithm>
#include <cmath>

namespace bpt::pricer::pricing {

std::optional<IvResult>
solve_iv(bool is_call, double market_price, double S, double K, double T, double r, uint32_t max_iter, double tol) {
    if (market_price <= 0.0 || S <= 0.0 || K <= 0.0 || T <= 0.0) {
        return std::nullopt;
    }

    // Check vs intrinsic — price must exceed (discounted) intrinsic value.
    // Black-76: intrinsic = df * max(0, F - K) for a call.
    const double df = std::exp(-r * T);
    const double intrinsic = is_call ? std::max(0.0, df * (S - K)) : std::max(0.0, df * (K - S));
    if (market_price < intrinsic - 1e-10) {
        return std::nullopt;  // Below intrinsic — bad data or arbitrage
    }

    // Initial guess: Brenner-Subrahmanyam approximation
    double sigma = std::sqrt(2.0 * M_PI / T) * (market_price / S);
    sigma = std::clamp(sigma, 0.01, 5.0);

    // Newton-Raphson with bisection fallback
    double lo = 0.001;
    double hi = 5.0;

    for (uint32_t i = 0; i < max_iter; ++i) {
        const auto bs = is_call ? bs_call(S, K, T, r, sigma) : bs_put(S, K, T, r, sigma);

        const double diff = bs.price - market_price;
        if (std::abs(diff) < tol) {
            return IvResult{sigma, i + 1};
        }

        // Narrow bisection bounds
        if (diff > 0.0) {
            hi = sigma;
        } else {
            lo = sigma;
        }

        // Newton step if vega is large enough
        if (bs.vega > 1e-12) {
            double new_sigma = sigma - diff / bs.vega;
            // Accept Newton step only if it stays within bounds
            if (new_sigma > lo && new_sigma < hi) {
                sigma = new_sigma;
                continue;
            }
        }

        // Fall back to bisection
        sigma = 0.5 * (lo + hi);
    }

    // Check if we're close enough after max iterations
    const auto final_bs = is_call ? bs_call(S, K, T, r, sigma) : bs_put(S, K, T, r, sigma);
    if (std::abs(final_bs.price - market_price) < tol * 100.0) {
        return IvResult{sigma, max_iter};
    }

    return std::nullopt;
}

}  // namespace bpt::pricer::pricing
