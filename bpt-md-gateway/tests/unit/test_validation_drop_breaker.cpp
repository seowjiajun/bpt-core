// Unit tests for ValidationDropBreaker (md-gateway side).

#include "md_gateway/md/validation_drop_breaker.h"

#include <gtest/gtest.h>

namespace {

using bpt::md_gateway::md::ValidationDropBreaker;

constexpr uint64_t kTick = 100'000'000ULL;  // 100ms — 100 events fit in 60s window

ValidationDropBreaker::Config make_cfg(double threshold_pct = 30.0,
                                       uint32_t min_events = 50,
                                       uint64_t window_ns = 60ULL * 1'000'000'000ULL) {
    ValidationDropBreaker::Config c;
    c.enabled = true;
    c.threshold_pct = threshold_pct;
    c.min_events = min_events;
    c.window_ns = window_ns;
    return c;
}

TEST(ValidationDropBreakerTest, DisabledRecordIsNoOp) {
    ValidationDropBreaker b({});  // enabled=false
    for (int i = 0; i < 500; ++i)
        b.record(/*is_drop=*/true, /*now_ns=*/i * kTick);
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.total_in_window(), 0u);
}

TEST(ValidationDropBreakerTest, BelowMinEventsNeverTrips) {
    ValidationDropBreaker b(make_cfg(/*threshold_pct=*/10.0, /*min_events=*/50));
    for (int i = 0; i < 49; ++i) b.record(true, i * kTick);
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.total_in_window(), 49u);
    EXPECT_EQ(b.drops_in_window(), 49u);
}

TEST(ValidationDropBreakerTest, BelowThresholdDoesNotTrip) {
    // 20% drops with a 30% threshold: integer counts oscillate around
    // the rolling rate, but the breaker uses strict-greater so a steady
    // stream that tops out near 20% must never trip. Using >>= threshold
    // headroom keeps the test insensitive to intermediate drift
    // (at i=49 with 10 drops so far the rate is 20%, not 30%).
    ValidationDropBreaker b(make_cfg(/*threshold_pct=*/30.0, /*min_events=*/50));
    for (int i = 0; i < 100; ++i) {
        const bool is_drop = (i % 5 == 4);  // 1/5 = 20%
        b.record(is_drop, i * kTick);
    }
    EXPECT_EQ(b.total_in_window(), 100u);
    EXPECT_EQ(b.drops_in_window(), 20u);
    EXPECT_FALSE(b.tripped());
}

TEST(ValidationDropBreakerTest, AboveThresholdTrips) {
    // 35% drops. Interleaving keeps running rate smooth; once total
    // passes min_events=50 and rate > 30%, it trips.
    ValidationDropBreaker b(make_cfg(/*threshold_pct=*/30.0, /*min_events=*/50));
    for (int i = 0; i < 100; ++i) {
        const bool is_drop = (i % 20 < 7);  // 7/20 = 35%
        b.record(is_drop, i * kTick);
    }
    EXPECT_TRUE(b.tripped());
}

TEST(ValidationDropBreakerTest, LatchesAfterTrip) {
    ValidationDropBreaker b(make_cfg(/*threshold_pct=*/30.0, /*min_events=*/10));
    // Short burst gets us above threshold fast.
    for (int i = 0; i < 20; ++i) b.record(true, i * kTick);
    ASSERT_TRUE(b.tripped());

    // Then a long stream of passes — window drains but latch stays.
    for (int i = 0; i < 1000; ++i) b.record(false, (10'000 + i) * kTick);
    EXPECT_TRUE(b.tripped()) << "Latch must stick; restart required";
}

TEST(ValidationDropBreakerTest, WindowEvictionDrainsOldDrops) {
    // Pre-trip (min_events=100 — the burst is too small to trip).
    // Verifies stale events age out of the window so counters reflect
    // the *current* rate. Config has min_events=100 so even 100% drop
    // in a short burst can't trip it, giving us a clean eviction test.
    const uint64_t window_ns = 1ULL * 1'000'000'000ULL;
    ValidationDropBreaker b(make_cfg(/*threshold_pct=*/50.0, /*min_events=*/100, window_ns));

    // Burst of 20 drops, all within 500ms. Below min_events, no trip.
    for (int i = 0; i < 20; ++i) b.record(true, i * 25'000'000ULL);
    ASSERT_FALSE(b.tripped());
    EXPECT_EQ(b.drops_in_window(), 20u);

    // Advance well past the 1s window with non-drops: cutoff pushes all
    // the original drops out. Counts should reflect only recent passes.
    for (int i = 0; i < 50; ++i)
        b.record(false, 2'000'000'000ULL + i * 50'000'000ULL);
    EXPECT_EQ(b.drops_in_window(), 0u);
    EXPECT_FALSE(b.tripped());
}

TEST(ValidationDropBreakerTest, NtpRegressionDoesNotUnderflow) {
    ValidationDropBreaker b(make_cfg(/*threshold_pct=*/30.0, /*min_events=*/10));
    for (int i = 0; i < 20; ++i) b.record(false, static_cast<uint64_t>(i));
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.total_in_window(), 20u);
}

}  // namespace
