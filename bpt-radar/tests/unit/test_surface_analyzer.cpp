// Pure-logic tests for the surface analyzer. Hand-built SurfacePoint vectors
// stand in for an SBE-decoded VolSurface — keeps the test free of Aeron deps.

#include "radar/analysis/surface_analyzer.h"

#include <cmath>
#include <gtest/gtest.h>

namespace {

using bpt::radar::analysis::atm_iv;
using bpt::radar::analysis::atm_skew_slope;
using bpt::radar::analysis::atm_strike;
using bpt::radar::analysis::kSideCall;
using bpt::radar::analysis::kSidePut;
using bpt::radar::analysis::risk_reversal_25d;
using bpt::radar::analysis::SurfacePoint;

SurfacePoint make_pt(double strike, int side, double iv, double delta = 0.0) {
    SurfacePoint p;
    p.strike_price = strike;
    p.option_side = side;
    p.implied_vol = iv;
    p.forward_price = 100.0;
    p.delta = delta;
    return p;
}

TEST(SurfaceAnalyzer, AtmStrikePicksClosestToForward) {
    std::vector<SurfacePoint> pts = {make_pt(90.0, kSideCall, 0.60),
                                     make_pt(105.0, kSideCall, 0.55),
                                     make_pt(115.0, kSideCall, 0.58)};
    EXPECT_DOUBLE_EQ(atm_strike(pts), 105.0);
}

TEST(SurfaceAnalyzer, AtmIvAveragesCallAndPutAtAtmStrike) {
    std::vector<SurfacePoint> pts = {make_pt(100.0, kSideCall, 0.50), make_pt(100.0, kSidePut, 0.60)};
    EXPECT_DOUBLE_EQ(atm_iv(pts), 0.55);
}

TEST(SurfaceAnalyzer, AtmIvFallsBackToSingleLegWhenOtherMissing) {
    std::vector<SurfacePoint> pts = {make_pt(100.0, kSideCall, 0.50)};
    EXPECT_DOUBLE_EQ(atm_iv(pts), 0.50);
}

TEST(SurfaceAnalyzer, RiskReversal25dInterpolatesByAbsDelta) {
    // Build call curve with |delta| in {0.10, 0.50} and put curve with |delta|
    // in {0.20, 0.30}. Interpolation at 0.25:
    //   call: at 0.25 between (0.10, 0.40) and (0.50, 0.60) → 0.40 + (0.25-0.10)/(0.50-0.10)*0.20 = 0.475
    //   put:  at 0.25 between (0.20, 0.55) and (0.30, 0.45) → 0.55 + (0.25-0.20)/(0.30-0.20)*(-0.10) = 0.50
    //   RR = 0.475 − 0.50 = −0.025
    std::vector<SurfacePoint> pts = {make_pt(110.0, kSideCall, 0.40, 0.10),
                                     make_pt(95.0, kSideCall, 0.60, 0.50),
                                     make_pt(105.0, kSidePut, 0.55, -0.20),
                                     make_pt(100.0, kSidePut, 0.45, -0.30)};
    EXPECT_NEAR(risk_reversal_25d(pts), -0.025, 1e-9);
}

TEST(SurfaceAnalyzer, RiskReversalNanIfLegMissing) {
    std::vector<SurfacePoint> pts = {make_pt(110.0, kSideCall, 0.40, 0.10), make_pt(95.0, kSideCall, 0.60, 0.50)};
    EXPECT_TRUE(std::isnan(risk_reversal_25d(pts)));
}

TEST(SurfaceAnalyzer, AtmSkewSlopeIsCentralLogStrikeDifference) {
    // Two strikes flanking forward=100: K=95 IV=0.55, K=105 IV=0.45.
    // slope = (0.45 − 0.55) / (ln(105) − ln(95)) = −0.10 / 0.10008 ≈ −0.99917
    std::vector<SurfacePoint> pts = {make_pt(95.0, kSideCall, 0.55), make_pt(105.0, kSideCall, 0.45)};
    const double expected = -0.10 / (std::log(105.0) - std::log(95.0));
    EXPECT_NEAR(atm_skew_slope(pts), expected, 1e-9);
}

TEST(SurfaceAnalyzer, AtmSkewSlopeNanIfNoBracket) {
    // Both strikes on the same side of the forward — no bracketing pair.
    std::vector<SurfacePoint> pts = {make_pt(110.0, kSideCall, 0.55), make_pt(120.0, kSideCall, 0.45)};
    EXPECT_TRUE(std::isnan(atm_skew_slope(pts)));
}

}  // namespace
