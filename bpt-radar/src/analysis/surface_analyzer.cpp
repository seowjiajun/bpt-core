#include "radar/analysis/surface_analyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace bpt::radar::analysis {
namespace {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

double forward_of(std::span<const SurfacePoint> points) {
    for (const auto& p : points)
        if (std::isfinite(p.forward_price) && p.forward_price > 0.0)
            return p.forward_price;
    return kNan;
}

}  // namespace

double atm_strike(std::span<const SurfacePoint> points) {
    const double fwd = forward_of(points);
    if (!std::isfinite(fwd))
        return kNan;

    double best_strike = kNan;
    double best_dist = std::numeric_limits<double>::infinity();
    for (const auto& p : points) {
        const double d = std::abs(p.strike_price - fwd);
        if (d < best_dist) {
            best_dist = d;
            best_strike = p.strike_price;
        }
    }
    return best_strike;
}

double atm_iv(std::span<const SurfacePoint> points) {
    const double k_atm = atm_strike(points);
    if (!std::isfinite(k_atm))
        return kNan;

    double call_iv = kNan;
    double put_iv = kNan;
    for (const auto& p : points) {
        if (p.strike_price != k_atm)
            continue;
        if (p.option_side == kSideCall && std::isfinite(p.implied_vol))
            call_iv = p.implied_vol;
        else if (p.option_side == kSidePut && std::isfinite(p.implied_vol))
            put_iv = p.implied_vol;
    }

    if (std::isfinite(call_iv) && std::isfinite(put_iv))
        return 0.5 * (call_iv + put_iv);
    if (std::isfinite(call_iv))
        return call_iv;
    if (std::isfinite(put_iv))
        return put_iv;
    return kNan;
}

double risk_reversal_25d(std::span<const SurfacePoint> points) {
    // Collect (|delta|, iv) for calls and puts separately. Then linearly
    // interpolate IV at |delta| = 0.25 for each leg. RR = IV_call − IV_put.
    constexpr double kTargetAbsDelta = 0.25;

    std::vector<std::pair<double, double>> call_curve;
    std::vector<std::pair<double, double>> put_curve;
    for (const auto& p : points) {
        if (!std::isfinite(p.delta) || !std::isfinite(p.implied_vol))
            continue;
        const double abs_d = std::abs(p.delta);
        if (abs_d <= 0.0 || abs_d >= 1.0)
            continue;
        if (p.option_side == kSideCall)
            call_curve.emplace_back(abs_d, p.implied_vol);
        else if (p.option_side == kSidePut)
            put_curve.emplace_back(abs_d, p.implied_vol);
    }

    auto interp = [](std::vector<std::pair<double, double>>& curve, double x) -> double {
        if (curve.size() < 2)
            return kNan;
        std::sort(curve.begin(), curve.end());
        // Find bracketing pair.
        for (size_t i = 0; i + 1 < curve.size(); ++i) {
            const auto& [x0, y0] = curve[i];
            const auto& [x1, y1] = curve[i + 1];
            if (x >= x0 && x <= x1) {
                if (x1 == x0)
                    return y0;
                const double t = (x - x0) / (x1 - x0);
                return y0 + t * (y1 - y0);
            }
        }
        return kNan;  // out of range — refuse to extrapolate
    };

    const double iv_call = interp(call_curve, kTargetAbsDelta);
    const double iv_put = interp(put_curve, kTargetAbsDelta);
    if (!std::isfinite(iv_call) || !std::isfinite(iv_put))
        return kNan;
    return iv_call - iv_put;
}

double atm_skew_slope(std::span<const SurfacePoint> points) {
    const double fwd = forward_of(points);
    if (!std::isfinite(fwd) || fwd <= 0.0)
        return kNan;

    // Collapse call+put at each strike to an average IV.
    struct StrikeIv {
        double k;
        double iv_sum;
        int n;
    };
    std::vector<StrikeIv> grid;
    for (const auto& p : points) {
        if (!std::isfinite(p.implied_vol) || p.strike_price <= 0.0)
            continue;
        auto it = std::find_if(grid.begin(), grid.end(), [&](const StrikeIv& s) { return s.k == p.strike_price; });
        if (it == grid.end())
            grid.push_back({p.strike_price, p.implied_vol, 1});
        else {
            it->iv_sum += p.implied_vol;
            ++it->n;
        }
    }
    if (grid.size() < 2)
        return kNan;
    std::sort(grid.begin(), grid.end(), [](const StrikeIv& a, const StrikeIv& b) { return a.k < b.k; });

    // Find the strike pair bracketing the forward; central difference between them.
    for (size_t i = 0; i + 1 < grid.size(); ++i) {
        if (grid[i].k <= fwd && grid[i + 1].k >= fwd) {
            const double iv0 = grid[i].iv_sum / grid[i].n;
            const double iv1 = grid[i + 1].iv_sum / grid[i + 1].n;
            const double dlogK = std::log(grid[i + 1].k) - std::log(grid[i].k);
            if (dlogK == 0.0)
                return kNan;
            return (iv1 - iv0) / dlogK;
        }
    }
    return kNan;
}

}  // namespace bpt::radar::analysis
