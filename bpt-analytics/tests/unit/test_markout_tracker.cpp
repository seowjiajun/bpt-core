#include "analytics/analysis/markout_tracker.h"

#include <gtest/gtest.h>

namespace {

using bpt::analytics::analysis::MarkoutTracker;

constexpr uint64_t SEC = 1'000'000'000ULL;
constexpr uint64_t INST = 100;

// ---------------------------------------------------------------------------
// Basic lifecycle
// ---------------------------------------------------------------------------

TEST(MarkoutTrackerTest, EmptyByDefault) {
    MarkoutTracker tracker;
    EXPECT_EQ(tracker.pending_count(), 0u);
    EXPECT_TRUE(tracker.consume().empty());
}

TEST(MarkoutTrackerTest, FillAddsToPending) {
    MarkoutTracker tracker;
    tracker.on_fill(INST, +1, 75000.0, 74999.5, 10 * SEC);
    EXPECT_EQ(tracker.pending_count(), 1u);
}

// ---------------------------------------------------------------------------
// Markout computation
// ---------------------------------------------------------------------------

TEST(MarkoutTrackerTest, BuyFillFavorableMarkout) {
    // Buy at mid=100. Price goes up to 100.10 → +10 bps favorable.
    MarkoutTracker tracker({.horizon_1_ns = SEC, .horizon_2_ns = 5 * SEC, .horizon_3_ns = 30 * SEC});

    tracker.on_fill(INST, +1, 100.0, 100.0, 10 * SEC);

    // Tick at +1s: mid=100.10
    tracker.on_tick(100.10, 11 * SEC);
    // Tick at +5s: mid=100.20
    tracker.on_tick(100.20, 15 * SEC);
    // Tick at +30s: mid=100.05
    int completed = tracker.on_tick(100.05, 40 * SEC);

    EXPECT_EQ(completed, 1);
    auto obs = tracker.consume();
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_EQ(obs[0].side_sign, +1);
    EXPECT_NEAR(obs[0].markout_1s_bps, 10.0, 0.1);    // +10 bps at 1s
    EXPECT_NEAR(obs[0].markout_5s_bps, 20.0, 0.1);    // +20 bps at 5s
    EXPECT_NEAR(obs[0].markout_30s_bps, 5.0, 0.1);    // +5 bps at 30s
}

TEST(MarkoutTrackerTest, SellFillAdverseMarkout) {
    // Sell at mid=100. Price goes UP to 100.30 → adverse for seller.
    MarkoutTracker tracker({.horizon_1_ns = SEC, .horizon_2_ns = 5 * SEC, .horizon_3_ns = 30 * SEC});

    tracker.on_fill(INST, -1, 100.0, 100.0, 10 * SEC);

    tracker.on_tick(100.30, 11 * SEC);
    tracker.on_tick(100.30, 15 * SEC);
    int completed = tracker.on_tick(100.30, 40 * SEC);

    EXPECT_EQ(completed, 1);
    auto obs = tracker.consume();
    ASSERT_EQ(obs.size(), 1u);
    // Sell, price went up → negative markout (adverse)
    EXPECT_LT(obs[0].markout_1s_bps, 0.0);
    EXPECT_NEAR(obs[0].markout_5s_bps, -30.0, 0.1);
}

TEST(MarkoutTrackerTest, BuyFillAdverseMarkout) {
    // Buy at mid=100. Price drops to 99.80 → adverse for buyer.
    MarkoutTracker tracker({.horizon_1_ns = SEC, .horizon_2_ns = 5 * SEC, .horizon_3_ns = 30 * SEC});

    tracker.on_fill(INST, +1, 100.0, 100.0, 10 * SEC);

    tracker.on_tick(99.80, 11 * SEC);
    tracker.on_tick(99.80, 15 * SEC);
    tracker.on_tick(99.80, 40 * SEC);

    auto obs = tracker.consume();
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_NEAR(obs[0].markout_5s_bps, -20.0, 0.1);
}

// ---------------------------------------------------------------------------
// Incremental horizon logging
// ---------------------------------------------------------------------------

TEST(MarkoutTrackerTest, HorizonsLoggedIncrementally) {
    MarkoutTracker tracker({.horizon_1_ns = SEC, .horizon_2_ns = 5 * SEC, .horizon_3_ns = 30 * SEC});

    tracker.on_fill(INST, +1, 100.0, 100.0, 10 * SEC);

    // Only 1s horizon crossed
    EXPECT_EQ(tracker.on_tick(100.10, 11 * SEC), 0);
    EXPECT_EQ(tracker.pending_count(), 1u);

    // 5s horizon crossed
    EXPECT_EQ(tracker.on_tick(100.20, 15 * SEC), 0);
    EXPECT_EQ(tracker.pending_count(), 1u);

    // 30s horizon crossed — now complete
    EXPECT_EQ(tracker.on_tick(100.05, 40 * SEC), 1);
    EXPECT_EQ(tracker.pending_count(), 0u);
}

// ---------------------------------------------------------------------------
// Multiple fills
// ---------------------------------------------------------------------------

TEST(MarkoutTrackerTest, MultipleFillsTrackedIndependently) {
    MarkoutTracker tracker({.horizon_1_ns = SEC, .horizon_2_ns = 2 * SEC, .horizon_3_ns = 3 * SEC});

    tracker.on_fill(INST, +1, 100.0, 100.0, 10 * SEC);
    tracker.on_fill(INST, -1, 100.0, 100.0, 11 * SEC);

    EXPECT_EQ(tracker.pending_count(), 2u);

    // At t=13s: first fill has all 3 horizons (3s elapsed), second has 1s+2s
    tracker.on_tick(100.10, 13 * SEC);
    EXPECT_EQ(tracker.pending_count(), 1u);

    // At t=14s: second fill completes
    tracker.on_tick(100.10, 14 * SEC);

    auto obs = tracker.consume();
    ASSERT_EQ(obs.size(), 2u);
    EXPECT_EQ(obs[0].side_sign, +1);
    EXPECT_EQ(obs[1].side_sign, -1);
}

// ---------------------------------------------------------------------------
// Max pending eviction
// ---------------------------------------------------------------------------

TEST(MarkoutTrackerTest, MaxPendingEvictsOldest) {
    MarkoutTracker tracker({.horizon_1_ns = SEC, .horizon_2_ns = SEC, .horizon_3_ns = SEC, .max_pending = 2});

    tracker.on_fill(INST, +1, 100.0, 100.0, 1 * SEC);  // will be evicted
    tracker.on_fill(INST, +1, 101.0, 101.0, 2 * SEC);
    tracker.on_fill(INST, -1, 102.0, 102.0, 3 * SEC);  // evicts first

    EXPECT_EQ(tracker.pending_count(), 2u);

    // Complete all remaining
    tracker.on_tick(103.0, 10 * SEC);
    auto obs = tracker.consume();
    ASSERT_EQ(obs.size(), 2u);
    EXPECT_DOUBLE_EQ(obs[0].fill_price, 101.0);
    EXPECT_DOUBLE_EQ(obs[1].fill_price, 102.0);
}

// ---------------------------------------------------------------------------
// Zero mid edge case
// ---------------------------------------------------------------------------

TEST(MarkoutTrackerTest, ZeroMidReturnsZeroMarkout) {
    MarkoutTracker tracker({.horizon_1_ns = SEC, .horizon_2_ns = SEC, .horizon_3_ns = SEC});

    tracker.on_fill(INST, +1, 100.0, 0.0, 1 * SEC);
    tracker.on_tick(100.0, 10 * SEC);

    auto obs = tracker.consume();
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_DOUBLE_EQ(obs[0].markout_1s_bps, 0.0);
}

}  // namespace
