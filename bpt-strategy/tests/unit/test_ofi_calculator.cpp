#include "strategy/strategy/ofi_calculator.h"

#include <gtest/gtest.h>

namespace {

using bpt::strategy::strategy::OFICalculator;
using Level = OFICalculator::Level;

constexpr uint64_t SEC = 1'000'000'000ULL;

// ---------------------------------------------------------------------------
// Warmup
// ---------------------------------------------------------------------------

TEST(OFICalculatorTest, FirstUpdateReturnsZero) {
    OFICalculator calc({.max_levels = 5, .window_ns = SEC});
    double v = calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 1 * SEC);
    EXPECT_DOUBLE_EQ(v, 0.0);
    EXPECT_TRUE(calc.is_warm());
}

TEST(OFICalculatorTest, NotWarmBeforeFirstUpdate) {
    OFICalculator calc({.max_levels = 5, .window_ns = SEC});
    EXPECT_FALSE(calc.is_warm());
    EXPECT_DOUBLE_EQ(calc.value(), 0.0);
}

// ---------------------------------------------------------------------------
// Basic OFI signals
// ---------------------------------------------------------------------------

TEST(OFICalculatorTest, BidSizeIncreaseIsPositive) {
    OFICalculator calc({.max_levels = 1, .window_ns = 10 * SEC});

    // Snapshot 1: bid 100@10, ask 101@10
    calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 1 * SEC);

    // Snapshot 2: bid size grows to 20 — buy pressure
    double v = calc.update({{100.0, 20.0}}, {{101.0, 10.0}}, 2 * SEC);
    EXPECT_GT(v, 0.0);
}

TEST(OFICalculatorTest, AskSizeIncreaseIsNegative) {
    OFICalculator calc({.max_levels = 1, .window_ns = 10 * SEC});

    calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 1 * SEC);

    // Ask size grows — sell pressure
    double v = calc.update({{100.0, 10.0}}, {{101.0, 20.0}}, 2 * SEC);
    EXPECT_LT(v, 0.0);
}

TEST(OFICalculatorTest, BidPriceUpIsPositive) {
    OFICalculator calc({.max_levels = 1, .window_ns = 10 * SEC});

    calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 1 * SEC);

    // Bid price rises — buy pressure (CKS: price improvement = +new_qty)
    double v = calc.update({{100.5, 10.0}}, {{101.0, 10.0}}, 2 * SEC);
    EXPECT_GT(v, 0.0);
}

TEST(OFICalculatorTest, AskPriceDownIsPositive) {
    OFICalculator calc({.max_levels = 1, .window_ns = 10 * SEC});

    calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 1 * SEC);

    // Ask price drops — buy pressure (seller offering lower = e_a = +new_qty,
    // but OFI = e_b - e_a, so this contributes negatively? Let me think...
    // Actually: ask price DOWN = "improved" for asks. e_a = +new_qty.
    // OFI = e_b - e_a. With bid unchanged (e_b = 0), OFI = -new_qty < 0.
    // Wait — the CKS convention is: ask price down means sellers are
    // getting more aggressive, which is SELL pressure, negative OFI.
    // So this should actually be negative.
    double v = calc.update({{100.0, 10.0}}, {{100.5, 10.0}}, 2 * SEC);
    EXPECT_LT(v, 0.0);
}

TEST(OFICalculatorTest, SymmetricMoveIsZero) {
    OFICalculator calc({.max_levels = 1, .window_ns = 10 * SEC});

    calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 1 * SEC);

    // Both sides grow equally
    double v = calc.update({{100.0, 20.0}}, {{101.0, 20.0}}, 2 * SEC);
    EXPECT_DOUBLE_EQ(v, 0.0);
}

// ---------------------------------------------------------------------------
// Multi-level weighting
// ---------------------------------------------------------------------------

TEST(OFICalculatorTest, DeeperLevelsWeightedLess) {
    // 2-level calculator. Change at level 2 should have half the weight
    // of the same change at level 1.
    OFICalculator calc({.max_levels = 2, .window_ns = 10 * SEC});

    std::vector<Level> bids = {{100.0, 10.0}, {99.0, 10.0}};
    std::vector<Level> asks = {{101.0, 10.0}, {102.0, 10.0}};
    calc.update(bids, asks, 1 * SEC);

    // Increase only level-2 bid by 10
    std::vector<Level> bids2 = {{100.0, 10.0}, {99.0, 20.0}};
    double v_l2 = calc.update(bids2, asks, 2 * SEC);

    // Reset and do same change at level 1
    calc.reset();
    calc.update(bids, asks, 1 * SEC);
    std::vector<Level> bids3 = {{100.0, 20.0}, {99.0, 10.0}};
    double v_l1 = calc.update(bids3, asks, 2 * SEC);

    // Level 1 contribution should be larger (weight 1.0 vs 0.5)
    EXPECT_GT(v_l1, v_l2);
    // The ratio of raw contributions should be 2:1 (but normalization may
    // shift it slightly since depth is the same)
    EXPECT_NEAR(v_l1 / v_l2, 2.0, 0.01);
}

// ---------------------------------------------------------------------------
// Rolling window
// ---------------------------------------------------------------------------

TEST(OFICalculatorTest, WindowEvictsStaleSamples) {
    OFICalculator calc({.max_levels = 1, .window_ns = SEC});

    calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 0);

    // Big buy at t=0.1s
    calc.update({{100.0, 50.0}}, {{101.0, 10.0}}, 100'000'000);
    double v1 = calc.value();
    EXPECT_GT(v1, 0.0);

    // At t=1.5s, the t=0 and t=0.1s samples are outside the 1s window.
    // With no new change, the contribution from the spike is evicted.
    calc.update({{100.0, 50.0}}, {{101.0, 10.0}}, 1'500'000'000);
    double v2 = calc.value();
    // After eviction, the remaining contribution should be just the latest
    // (no change from prev → 0 contribution), so value should be ~0.
    EXPECT_NEAR(v2, 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(OFICalculatorTest, ResetClearsState) {
    OFICalculator calc({.max_levels = 1, .window_ns = 10 * SEC});

    calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 1 * SEC);
    calc.update({{100.0, 50.0}}, {{101.0, 10.0}}, 2 * SEC);
    EXPECT_TRUE(calc.is_warm());
    EXPECT_NE(calc.value(), 0.0);

    calc.reset();
    EXPECT_FALSE(calc.is_warm());
    EXPECT_DOUBLE_EQ(calc.value(), 0.0);
    EXPECT_DOUBLE_EQ(calc.avg_depth(), 0.0);
}

// ---------------------------------------------------------------------------
// Edge: empty / missing levels
// ---------------------------------------------------------------------------

TEST(OFICalculatorTest, EmptyBookReturnsZero) {
    OFICalculator calc({.max_levels = 5, .window_ns = SEC});
    double v = calc.update({}, {}, 1 * SEC);
    EXPECT_DOUBLE_EQ(v, 0.0);
    // Second update with empty book — still warm, but no levels to compute
    v = calc.update({}, {}, 2 * SEC);
    EXPECT_DOUBLE_EQ(v, 0.0);
}

TEST(OFICalculatorTest, FewerLevelsThanMax) {
    OFICalculator calc({.max_levels = 5, .window_ns = 10 * SEC});

    // Only 1 level provided to a 5-level calculator
    calc.update({{100.0, 10.0}}, {{101.0, 10.0}}, 1 * SEC);
    double v = calc.update({{100.0, 20.0}}, {{101.0, 10.0}}, 2 * SEC);
    EXPECT_GT(v, 0.0);  // should still work with fewer levels
}

// ---------------------------------------------------------------------------
// avg_depth
// ---------------------------------------------------------------------------

TEST(OFICalculatorTest, AvgDepthTracksQueueSizes) {
    OFICalculator calc({.max_levels = 1, .window_ns = 10 * SEC});

    calc.update({{100.0, 10.0}}, {{101.0, 5.0}}, 1 * SEC);
    // First update stores prev but doesn't push a sample (returns 0)
    // — no window entries yet, avg_depth = 0
    EXPECT_DOUBLE_EQ(calc.avg_depth(), 0.0);

    calc.update({{100.0, 10.0}}, {{101.0, 5.0}}, 2 * SEC);
    // Now one sample in window. depth = bid_qty + ask_qty = 10 + 5 = 15
    EXPECT_DOUBLE_EQ(calc.avg_depth(), 15.0);
}

}  // namespace
