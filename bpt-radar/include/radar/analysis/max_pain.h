#pragma once

/// \file
/// \brief Max-pain strike calculator.
///
/// Max pain = the strike at which the *total in-the-money payout* to option
/// holders is minimised at expiry. Spot tends to gravitate toward this strike
/// in the last day or two of an expiry cycle ("pinning"), which is what makes
/// it a useful day-of-expiry color signal.
///
/// Algorithm: for each candidate spot S in the strike grid, compute
///   pain(S) = Σ_K max(S − K, 0) · OI_call_K  +  Σ_K max(K − S, 0) · OI_put_K
/// Return the S that minimises pain. Operates on a single expiry's strikes.
///
/// NaN return: fewer than one strike, or no strikes had finite OI.

#include "radar/analysis/surface_point.h"

#include <span>

namespace bpt::radar::analysis {

double max_pain_strike(std::span<const SurfacePoint> points);

}  // namespace bpt::radar::analysis
