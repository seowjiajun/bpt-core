#pragma once

#include <cstdint>

namespace bpt::pricer::pricing {

// Black-76 European option pricing (forward measure).
// All inputs in natural units (not fixed-point).
//
// F     = forward price to expiry (carry already embedded; for crypto this is
//         implied from the perp/dated-futures basis, not spot * exp(r*T))
// K     = strike price
// T     = time to expiry in years (ACT/365)
// r     = discount rate (annualised, continuous compounding); enters only via
//         the discount factor, NOT the drift — drift lives in F
// sigma = volatility (annualised)

struct BsResult {
    double price;
    double delta;  // w.r.t. forward
    double gamma;  // w.r.t. forward
    double vega;   // dPrice/dSigma — used by Newton-Raphson IV solver
    double theta;  // dPrice/dT (per year, negative for long options)
};

// Returns call price and forward greeks.
BsResult bs_call(double F, double K, double T, double r, double sigma);

// Returns put price and forward greeks.
BsResult bs_put(double F, double K, double T, double r, double sigma);

// Standard normal CDF (Abramowitz & Stegun approximation).
double norm_cdf(double x);

// Standard normal PDF.
double norm_pdf(double x);

}  // namespace bpt::pricer::pricing
