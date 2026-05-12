// Unit tests for the static helpers on PassiveMakerStrategy.
//
// We can't easily test the full strategy lifecycle here without standing
// up the IRefdataClient + IMdClient + OrderManager harness — that's an
// integration concern handled by bpt-backtester's smoke + sweep paths.
// What we *can* test in isolation: the math (microprice + tick rounding)
// and the drift-trigger predicate, both pure functions.

#include "strategy/strategy/passive_maker_strategy.h"

#include <gtest/gtest.h>

using bpt::strategy::strategy::PassiveMakerStrategy;

// ── Microprice ───────────────────────────────────────────────────────────────

TEST(PassiveMakerFairValue, BalancedBookEqualsMid) {
    // bid=100, ask=101, sizes equal → microprice = mid = 100.5
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::compute_fair_value(100.0, 101.0, 50.0, 50.0),
                     100.5);
}

TEST(PassiveMakerFairValue, HeavyBidLeansTowardAsk) {
    // Large bid size → microprice leans toward ask.
    // (100*10 + 101*100) / (100+10) = (1000 + 10100) / 110 = 11100/110 ≈ 100.909
    const double fv = PassiveMakerStrategy::compute_fair_value(100.0, 101.0,
                                                                /*bid_sz=*/100.0,
                                                                /*ask_sz=*/10.0);
    EXPECT_GT(fv, 100.5);
    EXPECT_LT(fv, 101.0);
    EXPECT_NEAR(fv, 100.909, 0.001);
}

TEST(PassiveMakerFairValue, HeavyAskLeansTowardBid) {
    // Large ask size → microprice leans toward bid.
    const double fv = PassiveMakerStrategy::compute_fair_value(100.0, 101.0,
                                                                /*bid_sz=*/10.0,
                                                                /*ask_sz=*/100.0);
    EXPECT_LT(fv, 100.5);
    EXPECT_GT(fv, 100.0);
}

TEST(PassiveMakerFairValue, ZeroSizesFallsBackToMid) {
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::compute_fair_value(100.0, 101.0, 0.0, 0.0),
                     100.5);
}

TEST(PassiveMakerFairValue, NoBookReturnsZero) {
    EXPECT_EQ(PassiveMakerStrategy::compute_fair_value(0.0, 101.0, 50.0, 50.0), 0.0);
    EXPECT_EQ(PassiveMakerStrategy::compute_fair_value(100.0, 0.0, 50.0, 50.0), 0.0);
}

// ── Tick rounding ────────────────────────────────────────────────────────────

TEST(PassiveMakerRoundToTick, RoundDownForBidStaysAtOrBelow) {
    // For a bid we want the rounded price ≤ input, within one tick.
    // FP imprecision can land on either floor or floor−1 for inputs that
    // are exactly at a tick; either is acceptable for a bid since the
    // place_side guard further pulls back if we'd cross.
    const double tick = 0.00001;
    const double v = PassiveMakerStrategy::round_to_tick(0.163875, tick, /*round_down=*/true);
    EXPECT_LE(v, 0.163875 + 1e-12);          // never above input
    EXPECT_GE(v, 0.163875 - 2 * tick);       // within two ticks (FP slack)
}

TEST(PassiveMakerRoundToTick, RoundUpForAskStaysAtOrAbove) {
    const double tick = 0.00001;
    const double v = PassiveMakerStrategy::round_to_tick(0.163875, tick, /*round_down=*/false);
    EXPECT_GE(v, 0.163875 - 1e-12);
    EXPECT_LE(v, 0.163875 + 2 * tick);
}

TEST(PassiveMakerRoundToTick, IntegerTickIsExact) {
    // Integer tick avoids FP gymnastics — exact behaviour can be asserted.
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::round_to_tick(123.4, 1.0, true),  123.0);
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::round_to_tick(123.4, 1.0, false), 124.0);
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::round_to_tick(125.0, 1.0, true),  125.0);
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::round_to_tick(125.0, 1.0, false), 125.0);
}

// ── Regime gating ───────────────────────────────────────────────────────────

TEST(PassiveMakerRegimeGating, ZeroMultiplierLeavesBaseUnchanged) {
    // mult=0 → effective param == base regardless of vol. This is the
    // back-compat path: existing configs without regime-gating params
    // get the fixed-param baseline.
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::scale_with_vol(25.0, 0.0, 50.0), 25.0);
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::scale_with_vol(50.0, 0.0, 100.0), 50.0);
}

TEST(PassiveMakerRegimeGating, ZeroVolLeavesBaseUnchanged) {
    // Before the estimator warms up, vol_bps=0 → effective = base.
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::scale_with_vol(25.0, 1.0, 0.0), 25.0);
}

TEST(PassiveMakerRegimeGating, LinearScaling) {
    // The whole point: out = base + mult × vol_bps. Direct check.
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::scale_with_vol(25.0, 1.0, 5.0),  30.0);
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::scale_with_vol(25.0, 1.0, 30.0), 55.0);
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::scale_with_vol(50.0, 0.5, 30.0), 65.0);
}

TEST(PassiveMakerRegimeGating, AnnualToPerMinuteBpsKnownPoints) {
    // 50% APR → 1-minute stdev ≈ 6.9 bps. (Sanity check the conversion math
    // so a future refactor doesn't silently break the unit-conversion factor.)
    EXPECT_NEAR(PassiveMakerStrategy::annualized_to_per_minute_bps(0.50), 6.89, 0.05);

    // 100% APR → ≈ 13.8 bps/min
    EXPECT_NEAR(PassiveMakerStrategy::annualized_to_per_minute_bps(1.00), 13.79, 0.05);

    // 200% APR (volatile alts) → ≈ 27.6 bps/min
    EXPECT_NEAR(PassiveMakerStrategy::annualized_to_per_minute_bps(2.00), 27.58, 0.10);
}

TEST(PassiveMakerRegimeGating, AnnualToPerMinuteBpsHandlesZeroAndNegative) {
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::annualized_to_per_minute_bps(0.0), 0.0);
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::annualized_to_per_minute_bps(-0.1), 0.0);
}

TEST(PassiveMakerRoundToTick, ZeroTickPassThrough) {
    // Unknown tick (0.0) — leave price untouched. Caller's responsibility
    // to avoid misalignment.
    EXPECT_DOUBLE_EQ(PassiveMakerStrategy::round_to_tick(0.16387, 0.0, true),
                     0.16387);
}
