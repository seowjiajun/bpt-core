#pragma once

/// \file
/// \brief Pure-function metrics derived from a single-expiry slice of an IV surface.
///
/// All inputs are sequences of SurfacePoint covering ONE (exchange, underlying,
/// expiry) bucket. Caller is responsible for bucketing — see RadarService.
///
/// NaN returns: the inputs were insufficient to compute the metric (fewer
/// than two strikes spanning the forward, no put or no call leg, etc.).

#include "radar/analysis/surface_point.h"

#include <cstdint>
#include <span>

namespace bpt::radar::analysis {

/// \brief Strike whose distance to the forward is smallest, considering both legs.
///
/// Returns the IvPoint's strike. NaN if `points` is empty or forward_price is
/// not finite.
double atm_strike(std::span<const SurfacePoint> points);

/// \brief Average of ATM call IV and ATM put IV.
///
/// Falls back to whichever leg exists if the other is absent. NaN if neither.
double atm_iv(std::span<const SurfacePoint> points);

/// \brief 25-delta risk reversal: IV(25Δ call) − IV(25Δ put).
///
/// Linear interpolation across strikes by absolute delta. Returns NaN if
/// either leg lacks bracketing strikes around |delta| = 0.25.
double risk_reversal_25d(std::span<const SurfacePoint> points);

/// \brief Skew slope: dIV / d(log-strike) at the ATM strike.
///
/// Uses a central difference over the two strikes flanking the forward.
/// Considers calls and puts together. NaN if fewer than two strikes.
double atm_skew_slope(std::span<const SurfacePoint> points);

}  // namespace bpt::radar::analysis
