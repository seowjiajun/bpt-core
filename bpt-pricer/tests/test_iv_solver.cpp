#include "pricer/pricing/black_scholes.h"
#include "pricer/pricing/iv_solver.h"

#include <cmath>
#include <gtest/gtest.h>

namespace bpt::pricer::pricing {

TEST(IvSolver, RoundTrip) {
    // Price a call with known sigma, then recover it
    const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, sigma = 0.25;
    auto bs = bs_call(S, K, T, r, sigma);

    auto result = solve_iv(true, bs.price, S, K, T, r);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->iv, sigma, 1e-6);
}

TEST(IvSolver, RoundTripPut) {
    const double S = 100.0, K = 110.0, T = 0.5, r = 0.03, sigma = 0.30;
    auto bs = bs_put(S, K, T, r, sigma);

    auto result = solve_iv(false, bs.price, S, K, T, r);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->iv, sigma, 1e-6);
}

TEST(IvSolver, DeepOtmPut) {
    // Deep OTM put — solver should still converge
    const double S = 100.0, K = 60.0, T = 0.25, r = 0.05, sigma = 0.35;
    auto bs = bs_put(S, K, T, r, sigma);

    auto result = solve_iv(false, bs.price, S, K, T, r);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->iv, sigma, 1e-4);
}

TEST(IvSolver, HighVol) {
    // BTC-like: 80% vol
    const double S = 70000.0, K = 70000.0, T = 30.0 / 365.0, r = 0.05, sigma = 0.80;
    auto bs = bs_call(S, K, T, r, sigma);

    auto result = solve_iv(true, bs.price, S, K, T, r);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->iv, sigma, 1e-5);
}

TEST(IvSolver, LowVol) {
    const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, sigma = 0.05;
    auto bs = bs_call(S, K, T, r, sigma);

    auto result = solve_iv(true, bs.price, S, K, T, r);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->iv, sigma, 1e-5);
}

TEST(IvSolver, ZeroPriceReturnsNullopt) {
    auto result = solve_iv(true, 0.0, 100.0, 100.0, 1.0, 0.05);
    EXPECT_FALSE(result.has_value());
}

TEST(IvSolver, NegativePriceReturnsNullopt) {
    auto result = solve_iv(true, -1.0, 100.0, 100.0, 1.0, 0.05);
    EXPECT_FALSE(result.has_value());
}

TEST(IvSolver, ExpiredOptionReturnsNullopt) {
    auto result = solve_iv(true, 5.0, 100.0, 100.0, 0.0, 0.05);
    EXPECT_FALSE(result.has_value());
}

TEST(IvSolver, MultipleStrikesSameExpiry) {
    // Simulate a vol smile: ATM has lower vol than wings
    const double S = 100.0, T = 0.25, r = 0.05;

    struct StrikeVol {
        double K;
        double sigma;
    };
    StrikeVol cases[] = {
        {80.0, 0.35},
        {90.0, 0.28},
        {100.0, 0.22},
        {110.0, 0.27},
        {120.0, 0.34},
    };

    for (const auto& c : cases) {
        auto bs = bs_call(S, c.K, T, r, c.sigma);
        auto result = solve_iv(true, bs.price, S, c.K, T, r);
        ASSERT_TRUE(result.has_value()) << "Failed for K=" << c.K;
        EXPECT_NEAR(result->iv, c.sigma, 1e-5) << "Mismatch for K=" << c.K;
    }
}

}  // namespace bpt::pricer::pricing
