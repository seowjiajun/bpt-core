#include "pricer/pricing/black_scholes.h"

#include <cmath>
#include <gtest/gtest.h>

namespace bpt::pricer::pricing {

TEST(BlackScholes, NormCdfSymmetry) {
    EXPECT_NEAR(norm_cdf(0.0), 0.5, 1e-7);
    EXPECT_NEAR(norm_cdf(1.0) + norm_cdf(-1.0), 1.0, 1e-7);
    EXPECT_NEAR(norm_cdf(2.0) + norm_cdf(-2.0), 1.0, 1e-7);
}

TEST(BlackScholes, NormCdfKnownValues) {
    EXPECT_NEAR(norm_cdf(1.0), 0.8413447, 1e-6);
    EXPECT_NEAR(norm_cdf(-1.0), 0.1586553, 1e-6);
    EXPECT_NEAR(norm_cdf(1.96), 0.975002, 1e-5);
}

TEST(BlackScholes, NormCdfExtremes) {
    EXPECT_DOUBLE_EQ(norm_cdf(-10.0), 0.0);
    EXPECT_DOUBLE_EQ(norm_cdf(10.0), 1.0);
}

TEST(BlackScholes, CallPutParity) {
    // Black-76 put-call parity: C - P = df * (F - K)
    const double F = 100.0, K = 100.0, T = 1.0, r = 0.05, sigma = 0.2;

    auto call = bs_call(F, K, T, r, sigma);
    auto put = bs_put(F, K, T, r, sigma);

    const double parity_diff = call.price - put.price;
    const double expected = std::exp(-r * T) * (F - K);

    EXPECT_NEAR(parity_diff, expected, 1e-10);
}

TEST(BlackScholes, AtmCallPrice) {
    // Black-76 ATM call at F=K=100, T=1y, r=5%, sigma=20%
    // df*F*(N(0.1)-N(-0.1)) = 0.951229*100*0.0796557 ≈ 7.5774
    auto result = bs_call(100.0, 100.0, 1.0, 0.05, 0.2);
    EXPECT_NEAR(result.price, 7.5774, 0.01);
    EXPECT_GT(result.delta, 0.5);  // ATM call delta (df*N(d1)) > 0.5
    EXPECT_GT(result.vega, 0.0);
}

TEST(BlackScholes, DeepItmCall) {
    // Deep ITM call approaches discounted intrinsic; forward delta -> df.
    const double r = 0.05, T = 0.25;
    auto result = bs_call(200.0, 100.0, T, r, 0.2);
    const double df = std::exp(-r * T);
    const double intrinsic = df * (200.0 - 100.0);
    EXPECT_GT(result.price, intrinsic);
    EXPECT_NEAR(result.delta, df, 0.01);
}

TEST(BlackScholes, DeepOtmCall) {
    // Deep OTM call approaches zero
    auto result = bs_call(50.0, 100.0, 0.1, 0.05, 0.2);
    EXPECT_NEAR(result.price, 0.0, 0.01);
    EXPECT_NEAR(result.delta, 0.0, 0.01);
}

TEST(BlackScholes, ZeroInputs) {
    // Edge cases
    EXPECT_EQ(bs_call(0.0, 100.0, 1.0, 0.05, 0.2).price, 0.0);
    EXPECT_EQ(bs_call(100.0, 100.0, 0.0, 0.05, 0.2).price, 0.0);
    EXPECT_EQ(bs_call(100.0, 100.0, 1.0, 0.05, 0.0).price, 0.0);
}

TEST(BlackScholes, VegaPositive) {
    // Vega is always positive
    auto call = bs_call(100.0, 100.0, 1.0, 0.05, 0.3);
    auto put = bs_put(100.0, 100.0, 1.0, 0.05, 0.3);
    EXPECT_GT(call.vega, 0.0);
    EXPECT_GT(put.vega, 0.0);
    // Vega is the same for call and put
    EXPECT_NEAR(call.vega, put.vega, 1e-10);
}

TEST(BlackScholes, HighVolCall) {
    // High vol — BTC-like (80% annual vol)
    auto result = bs_call(70000.0, 70000.0, 30.0 / 365.0, 0.05, 0.8);
    EXPECT_GT(result.price, 0.0);
    EXPECT_GT(result.vega, 0.0);
}

TEST(BlackScholes, GammaSameForCallPut) {
    auto call = bs_call(100.0, 100.0, 1.0, 0.05, 0.2);
    auto put = bs_put(100.0, 100.0, 1.0, 0.05, 0.2);
    EXPECT_GT(call.gamma, 0.0);
    EXPECT_NEAR(call.gamma, put.gamma, 1e-10);
}

TEST(BlackScholes, ThetaNegativeForLongOptions) {
    // Theta is negative (time decay hurts long positions)
    auto call = bs_call(100.0, 100.0, 1.0, 0.05, 0.2);
    auto put = bs_put(100.0, 100.0, 1.0, 0.05, 0.2);
    EXPECT_LT(call.theta, 0.0);
    EXPECT_LT(put.theta, 0.0);
}

TEST(BlackScholes, GammaHighestAtm) {
    // Gamma peaks at ATM, lower for ITM/OTM
    auto atm = bs_call(100.0, 100.0, 0.25, 0.05, 0.2);
    auto itm = bs_call(100.0, 80.0, 0.25, 0.05, 0.2);
    auto otm = bs_call(100.0, 120.0, 0.25, 0.05, 0.2);
    EXPECT_GT(atm.gamma, itm.gamma);
    EXPECT_GT(atm.gamma, otm.gamma);
}

TEST(BlackScholes, CallPutDeltaRelation) {
    // Black-76 forward delta: call - put = df (= e^{-rT})
    const double r = 0.05, T = 1.0;
    auto call = bs_call(100.0, 100.0, T, r, 0.2);
    auto put = bs_put(100.0, 100.0, T, r, 0.2);
    EXPECT_NEAR(call.delta - put.delta, std::exp(-r * T), 1e-10);
}

}  // namespace bpt::pricer::pricing
