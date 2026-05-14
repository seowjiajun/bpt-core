#include "radar/analysis/max_pain.h"

#include <cmath>
#include <gtest/gtest.h>

namespace {

using bpt::radar::analysis::kSideCall;
using bpt::radar::analysis::kSidePut;
using bpt::radar::analysis::max_pain_strike;
using bpt::radar::analysis::SurfacePoint;

SurfacePoint pt(double strike, int side, double oi) {
    SurfacePoint p;
    p.strike_price = strike;
    p.option_side = side;
    p.open_interest = oi;
    return p;
}

TEST(MaxPain, PicksStrikeMinimizingItmPayout) {
    // Three strikes: 90, 100, 110. Heavy put OI at 110 means the ITM payout
    // is large for any spot below 110. Heavy call OI at 90 means ITM payout
    // is large for any spot above 90. The strike that splits the difference
    // — minimising total payout — is the middle: 100.
    std::vector<SurfacePoint> pts = {
        pt(90.0, kSideCall, 1000.0),  // ITM when S > 90
        pt(100.0, kSideCall, 100.0),
        pt(110.0, kSideCall, 100.0),
        pt(90.0, kSidePut, 100.0),
        pt(100.0, kSidePut, 100.0),
        pt(110.0, kSidePut, 1000.0),  // ITM when S < 110
    };
    EXPECT_DOUBLE_EQ(max_pain_strike(pts), 100.0);
}

TEST(MaxPain, IgnoresStrikesWithNanOi) {
    // Only one usable strike → returned as max-pain trivially.
    std::vector<SurfacePoint> pts = {pt(90.0, kSideCall, std::numeric_limits<double>::quiet_NaN()),
                                     pt(100.0, kSideCall, 1000.0)};
    EXPECT_DOUBLE_EQ(max_pain_strike(pts), 100.0);
}

TEST(MaxPain, EmptyInputReturnsNan) {
    std::vector<SurfacePoint> pts;
    EXPECT_TRUE(std::isnan(max_pain_strike(pts)));
}

}  // namespace
