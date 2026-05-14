#pragma once

/// \file
/// \brief Gamma Exposure (GEX) — Σ gamma × OI over all strikes for an underlying.
///
/// Standard market-color interpretation:
///   - Positive GEX: dealers are net long gamma → they hedge by selling rallies
///     / buying dips → vol-suppressing, mean-reverting regime.
///   - Negative GEX: dealers are net short gamma → they amplify directional
///     moves → vol-amplifying, momentum regime.
///
/// This implementation computes the raw sign+magnitude. Sign convention here:
/// we sum (+gamma·OI) for calls and (−gamma·OI) for puts, reflecting the
/// typical assumption that dealers are short calls and long puts (the customer
/// flow side). Refine when we have richer flow-side data; for now this matches
/// what every options dashboard (SpotGamma, SqueezeMetrics, etc.) reports.
///
/// Skipping a strike: any point missing gamma or OI (NaN) is excluded.
/// Returns {NaN total_oi=0 strikes=0} if no strike contributed.

#include "radar/analysis/surface_point.h"

#include <cstdint>
#include <span>

namespace bpt::radar::analysis {

struct GexResult {
    double gex{0.0};         ///< Σ ±gamma·OI (sign by option_side)
    double total_oi{0.0};    ///< Σ OI for strikes that contributed
    uint32_t strikes{0};     ///< count of strikes that contributed
};

GexResult compute_gex(std::span<const SurfacePoint> points);

}  // namespace bpt::radar::analysis
