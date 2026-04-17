#pragma once

#include <cstdint>
#include <optional>

namespace bpt::pricer::pricing {

struct IvResult {
    double iv;            // Annualised implied volatility
    uint32_t iterations;  // Newton-Raphson iterations used
};

// Solve for implied volatility using Newton-Raphson on the BS vega.
//
// is_call = true for calls, false for puts
// market_price = observed option price
// S = forward/spot price
// K = strike
// T = time to expiry (years)
// r = risk-free rate
// max_iter = maximum Newton-Raphson iterations
// tol = convergence tolerance on price difference
//
// Returns nullopt if the solver does not converge or the market price
// is below intrinsic (arbitrage / bad data).
std::optional<IvResult> solve_iv(bool is_call,
                                 double market_price,
                                 double S,
                                 double K,
                                 double T,
                                 double r,
                                 uint32_t max_iter = 100,
                                 double tol = 1e-8);

}  // namespace bpt::pricer::pricing
