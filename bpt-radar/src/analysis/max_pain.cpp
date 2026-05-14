#include "radar/analysis/max_pain.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace bpt::radar::analysis {

double max_pain_strike(std::span<const SurfacePoint> points) {
    // Filter to strikes with finite OI; collect unique strike grid.
    std::vector<double> strikes;
    strikes.reserve(points.size());
    for (const auto& p : points) {
        if (!std::isfinite(p.open_interest) || p.strike_price <= 0.0)
            continue;
        if (std::find(strikes.begin(), strikes.end(), p.strike_price) == strikes.end())
            strikes.push_back(p.strike_price);
    }
    if (strikes.empty())
        return std::numeric_limits<double>::quiet_NaN();
    std::sort(strikes.begin(), strikes.end());

    double best_strike = strikes.front();
    double best_pain = std::numeric_limits<double>::infinity();
    for (const double S : strikes) {
        double pain = 0.0;
        for (const auto& p : points) {
            if (!std::isfinite(p.open_interest))
                continue;
            if (p.option_side == kSideCall) {
                const double payout = S - p.strike_price;
                if (payout > 0.0)
                    pain += payout * p.open_interest;
            } else {
                const double payout = p.strike_price - S;
                if (payout > 0.0)
                    pain += payout * p.open_interest;
            }
        }
        if (pain < best_pain) {
            best_pain = pain;
            best_strike = S;
        }
    }
    return best_strike;
}

}  // namespace bpt::radar::analysis
