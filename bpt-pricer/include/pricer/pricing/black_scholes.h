#pragma once

#include <cstdint>

namespace bpt::pricer::pricing {

// Black-Scholes European option pricing.
// All inputs in natural units (not fixed-point).
//
// S     = spot/forward price
// K     = strike price
// T     = time to expiry in years (ACT/365)
// r     = risk-free rate (annualised, continuous compounding)
// sigma = volatility (annualised)

struct BsResult {
    double price;
    double delta;
    double gamma;
    double vega;   // dPrice/dSigma — used by Newton-Raphson IV solver
    double theta;  // dPrice/dT (per year, negative for long options)
};

// Returns call price, delta, vega.
BsResult bs_call(double S, double K, double T, double r, double sigma);

// Returns put price, delta, vega.
BsResult bs_put(double S, double K, double T, double r, double sigma);

// Standard normal CDF (Abramowitz & Stegun approximation).
double norm_cdf(double x);

// Standard normal PDF.
double norm_pdf(double x);

}  // namespace bpt::pricer::pricing
