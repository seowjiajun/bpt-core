#include "radar/analysis/gex.h"

#include <cmath>

namespace bpt::radar::analysis {

GexResult compute_gex(std::span<const SurfacePoint> points) {
    GexResult r;
    bool any = false;
    for (const auto& p : points) {
        if (!std::isfinite(p.gamma) || !std::isfinite(p.open_interest))
            continue;
        const double sign = (p.option_side == kSideCall) ? +1.0 : -1.0;
        r.gex += sign * p.gamma * p.open_interest;
        r.total_oi += p.open_interest;
        ++r.strikes;
        any = true;
    }
    if (!any)
        r.gex = std::numeric_limits<double>::quiet_NaN();
    return r;
}

}  // namespace bpt::radar::analysis
