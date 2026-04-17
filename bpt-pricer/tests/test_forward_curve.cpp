#include "pricer/pricing/forward_curve.h"

#include <cmath>
#include <gtest/gtest.h>

namespace bpt::pricer::pricing {

TEST(ForwardCurve, TimeToExpiry) {
    // 2026-01-01 to 2026-07-01 = 181 days = 0.4959 years
    double T = ForwardCurve::time_to_expiry(20260701, 20260101);
    EXPECT_NEAR(T, 181.0 / 365.0, 1e-6);
}

TEST(ForwardCurve, TimeToExpirySameDay) {
    double T = ForwardCurve::time_to_expiry(20260315, 20260315);
    EXPECT_DOUBLE_EQ(T, 0.0);
}

TEST(ForwardCurve, TimeToExpiryPast) {
    double T = ForwardCurve::time_to_expiry(20260101, 20260701);
    EXPECT_LT(T, 0.0);
}

TEST(ForwardCurve, SyntheticForward) {
    // F = S * exp(r * T)
    ForwardCurve fc;
    fc.set_spot(100.0);
    fc.set_risk_free_rate(0.05);

    double fwd = fc.get_forward(20270101, 20260101);
    double T = ForwardCurve::time_to_expiry(20270101, 20260101);
    double expected = 100.0 * std::exp(0.05 * T);

    EXPECT_NEAR(fwd, expected, 1e-6);
}

TEST(ForwardCurve, ExplicitForwardOverridesSynthetic) {
    ForwardCurve fc;
    fc.set_spot(100.0);
    fc.set_risk_free_rate(0.05);
    fc.set_forward(20270101, 108.0);

    double fwd = fc.get_forward(20270101, 20260101);
    EXPECT_DOUBLE_EQ(fwd, 108.0);
}

TEST(ForwardCurve, FallbackToSyntheticForUnknownExpiry) {
    ForwardCurve fc;
    fc.set_spot(100.0);
    fc.set_risk_free_rate(0.05);
    fc.set_forward(20270101, 108.0);

    // Query a different expiry — should fall back to synthetic
    double fwd = fc.get_forward(20260701, 20260101);
    double T = ForwardCurve::time_to_expiry(20260701, 20260101);
    double expected = 100.0 * std::exp(0.05 * T);

    EXPECT_NEAR(fwd, expected, 1e-6);
}

TEST(ForwardCurve, ZeroSpotReturnsZero) {
    ForwardCurve fc;
    fc.set_spot(0.0);
    double fwd = fc.get_forward(20270101, 20260101);
    EXPECT_DOUBLE_EQ(fwd, 0.0);
}

}  // namespace bpt::pricer::pricing
