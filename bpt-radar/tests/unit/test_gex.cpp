#include "radar/analysis/gex.h"

#include <cmath>
#include <gtest/gtest.h>

namespace {

using bpt::radar::analysis::compute_gex;
using bpt::radar::analysis::kSideCall;
using bpt::radar::analysis::kSidePut;
using bpt::radar::analysis::SurfacePoint;

SurfacePoint pt(int side, double gamma, double oi) {
    SurfacePoint p;
    p.option_side = side;
    p.gamma = gamma;
    p.open_interest = oi;
    return p;
}

TEST(Gex, SumsSignedGammaTimesOi) {
    // Call: +0.01·1000 = +10. Put: −0.02·500 = −10. Net = 0.
    std::vector<SurfacePoint> pts = {pt(kSideCall, 0.01, 1000.0), pt(kSidePut, 0.02, 500.0)};
    const auto r = compute_gex(pts);
    EXPECT_DOUBLE_EQ(r.gex, 0.0);
    EXPECT_DOUBLE_EQ(r.total_oi, 1500.0);
    EXPECT_EQ(r.strikes, 2u);
}

TEST(Gex, SkipsStrikesWithNanGamma) {
    std::vector<SurfacePoint> pts = {pt(kSideCall, std::numeric_limits<double>::quiet_NaN(), 1000.0),
                                     pt(kSideCall, 0.01, 500.0)};
    const auto r = compute_gex(pts);
    EXPECT_DOUBLE_EQ(r.gex, 5.0);
    EXPECT_DOUBLE_EQ(r.total_oi, 500.0);
    EXPECT_EQ(r.strikes, 1u);
}

TEST(Gex, SkipsStrikesWithNanOi) {
    std::vector<SurfacePoint> pts = {pt(kSideCall, 0.01, std::numeric_limits<double>::quiet_NaN()),
                                     pt(kSideCall, 0.02, 500.0)};
    const auto r = compute_gex(pts);
    EXPECT_DOUBLE_EQ(r.gex, 10.0);
    EXPECT_EQ(r.strikes, 1u);
}

TEST(Gex, EmptyInputReturnsNan) {
    std::vector<SurfacePoint> pts;
    const auto r = compute_gex(pts);
    EXPECT_TRUE(std::isnan(r.gex));
    EXPECT_DOUBLE_EQ(r.total_oi, 0.0);
    EXPECT_EQ(r.strikes, 0u);
}

}  // namespace
