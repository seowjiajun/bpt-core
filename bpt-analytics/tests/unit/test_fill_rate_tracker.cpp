#include "analytics/analysis/fill_rate_tracker.h"

#include <cmath>
#include <gtest/gtest.h>

namespace {

using bpt::analytics::analysis::FillRateTracker;

constexpr uint64_t MS = 1'000'000ULL;
constexpr uint64_t SEC = 1'000'000'000ULL;

// ---------------------------------------------------------------------------
// Empty state
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, EmptyReturnsNaN) {
    FillRateTracker tracker;
    auto s = tracker.stats(+1);
    EXPECT_TRUE(std::isnan(s.fill_rate));
    EXPECT_TRUE(std::isnan(s.mean_ttf_ms));
    EXPECT_EQ(s.fills, 0u);
    EXPECT_EQ(s.cancels, 0u);
    EXPECT_EQ(s.total, 0u);
}

// ---------------------------------------------------------------------------
// Single fill
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, SingleFill) {
    FillRateTracker tracker;
    tracker.on_acked(1, +1, 10 * SEC);
    tracker.on_filled(1, 10 * SEC + 500 * MS);  // 500ms TTF

    auto s = tracker.stats(+1);
    EXPECT_DOUBLE_EQ(s.fill_rate, 1.0);
    EXPECT_NEAR(s.mean_ttf_ms, 500.0, 1.0);
    EXPECT_EQ(s.fills, 1u);
    EXPECT_EQ(s.cancels, 0u);
}

// ---------------------------------------------------------------------------
// Single cancel
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, SingleCancel) {
    FillRateTracker tracker;
    tracker.on_acked(1, -1, 10 * SEC);
    tracker.on_cancelled(1, 11 * SEC);

    auto s = tracker.stats(-1);
    EXPECT_DOUBLE_EQ(s.fill_rate, 0.0);
    EXPECT_TRUE(std::isnan(s.mean_ttf_ms));  // no fills → no TTF
    EXPECT_EQ(s.fills, 0u);
    EXPECT_EQ(s.cancels, 1u);
}

// ---------------------------------------------------------------------------
// Mixed fills and cancels
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, MixedFillsAndCancels) {
    FillRateTracker tracker;
    // 3 fills, 2 cancels on bid side
    tracker.on_acked(1, +1, 1 * SEC);
    tracker.on_filled(1, 1 * SEC + 200 * MS);

    tracker.on_acked(2, +1, 2 * SEC);
    tracker.on_cancelled(2, 3 * SEC);

    tracker.on_acked(3, +1, 4 * SEC);
    tracker.on_filled(3, 4 * SEC + 800 * MS);

    tracker.on_acked(4, +1, 5 * SEC);
    tracker.on_cancelled(4, 6 * SEC);

    tracker.on_acked(5, +1, 7 * SEC);
    tracker.on_filled(5, 7 * SEC + 300 * MS);

    auto s = tracker.stats(+1);
    EXPECT_NEAR(s.fill_rate, 0.6, 0.01);  // 3/5
    EXPECT_NEAR(s.mean_ttf_ms, (200 + 800 + 300) / 3.0, 1.0);
    EXPECT_EQ(s.fills, 3u);
    EXPECT_EQ(s.cancels, 2u);
}

// ---------------------------------------------------------------------------
// Sides are independent
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, SidesIndependent) {
    FillRateTracker tracker;
    tracker.on_acked(1, +1, 1 * SEC);
    tracker.on_filled(1, 2 * SEC);

    tracker.on_acked(2, -1, 1 * SEC);
    tracker.on_cancelled(2, 2 * SEC);

    auto bid = tracker.stats(+1);
    auto ask = tracker.stats(-1);
    EXPECT_DOUBLE_EQ(bid.fill_rate, 1.0);
    EXPECT_DOUBLE_EQ(ask.fill_rate, 0.0);
}

// ---------------------------------------------------------------------------
// Unknown order_id ignored
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, UnknownOrderIgnored) {
    FillRateTracker tracker;
    tracker.on_filled(999, 1 * SEC);  // no matching ack
    tracker.on_cancelled(888, 1 * SEC);

    auto s = tracker.stats(+1);
    EXPECT_EQ(s.total, 0u);
}

// ---------------------------------------------------------------------------
// Window eviction
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, WindowEvicts) {
    FillRateTracker tracker({.window_size = 3});

    // First 3 are cancels
    for (uint64_t i = 1; i <= 3; ++i) {
        tracker.on_acked(i, +1, i * SEC);
        tracker.on_cancelled(i, (i + 1) * SEC);
    }
    EXPECT_DOUBLE_EQ(tracker.stats(+1).fill_rate, 0.0);

    // Next 3 are fills — evict the cancels
    for (uint64_t i = 4; i <= 6; ++i) {
        tracker.on_acked(i, +1, i * SEC);
        tracker.on_filled(i, i * SEC + 100 * MS);
    }
    EXPECT_DOUBLE_EQ(tracker.stats(+1).fill_rate, 1.0);  // only fills in window
    EXPECT_EQ(tracker.stats(+1).fills, 3u);
}

// ---------------------------------------------------------------------------
// Pending count
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, PendingCount) {
    FillRateTracker tracker;
    tracker.on_acked(1, +1, 1 * SEC);
    tracker.on_acked(2, -1, 2 * SEC);
    EXPECT_EQ(tracker.pending_count(), 2u);

    tracker.on_filled(1, 3 * SEC);
    EXPECT_EQ(tracker.pending_count(), 1u);

    tracker.on_cancelled(2, 4 * SEC);
    EXPECT_EQ(tracker.pending_count(), 0u);
}

// ---------------------------------------------------------------------------
// TTF precision
// ---------------------------------------------------------------------------

TEST(FillRateTrackerTest, TtfPrecision) {
    FillRateTracker tracker;
    tracker.on_acked(1, +1, 0);
    tracker.on_filled(1, 1'500'000ULL);  // 1.5ms

    auto s = tracker.stats(+1);
    EXPECT_NEAR(s.mean_ttf_ms, 1.5, 0.01);
}

}  // namespace
