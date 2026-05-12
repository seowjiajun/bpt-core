// Unit tests for RejectRateBreaker.
//
// Exercises the rolling-window rejection-rate tracker in isolation — the
// OrderProcessor glue is tested separately in test_order_processor_risk_latch.cpp.

#include "order_gateway/risk/reject_rate_breaker.h"

#include <gtest/gtest.h>

namespace {

using bpt::order_gateway::risk::RejectRateBreaker;

// 1s in ns, for readable timestamps below.
constexpr uint64_t kSec = 1'000'000'000ULL;
// 100ms — keeps a burst of 100 events inside the 60s default window so
// tests that care about aggregate counts aren't accidentally truncated.
constexpr uint64_t kTick = 100'000'000ULL;

RejectRateBreaker::Config make_cfg(double threshold_pct = 20.0,
                                   uint32_t min_events = 10,
                                   uint64_t window_ns = 60 * kSec) {
    RejectRateBreaker::Config c;
    c.enabled = true;
    c.threshold_pct = threshold_pct;
    c.min_events = min_events;
    c.window_ns = window_ns;
    return c;
}

TEST(RejectRateBreakerTest, DisabledRecordIsNoOp) {
    RejectRateBreaker::Config c;  // enabled=false
    RejectRateBreaker b(c);
    for (int i = 0; i < 1000; ++i)
        b.record(/*is_reject=*/true, /*now_ns=*/i * kSec);
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.total_in_window(), 0u);
    EXPECT_EQ(b.rejects_in_window(), 0u);
}

TEST(RejectRateBreakerTest, BelowMinEventsNeverTrips) {
    // threshold=10%, min_events=20. Feed 19 rejects → 100% rate but
    // sample size is below the guard, so the latch must stay open.
    RejectRateBreaker b(make_cfg(/*threshold_pct=*/10.0, /*min_events=*/20));
    for (int i = 0; i < 19; ++i)
        b.record(true, i * kSec);
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.total_in_window(), 19u);
    EXPECT_EQ(b.rejects_in_window(), 19u);
}

TEST(RejectRateBreakerTest, AtThresholdDoesNotTrip) {
    // rate_pct > threshold_pct is the trip condition, so exactly equal
    // (20/100 = 20%) should hold. Tests the strict-greater comparison.
    // The breaker evaluates on every record(), so we must interleave —
    // front-loading rejects would trip at min_events=10 long before the
    // full 100-event mix lands. Pattern "NNNNR" ensures 1/5 = 20% at
    // every multiple of 5.
    RejectRateBreaker b(make_cfg(/*threshold_pct=*/20.0, /*min_events=*/10));
    for (int i = 0; i < 100; ++i) {
        const bool is_reject = (i % 5 == 4);  // every 5th event is a reject
        b.record(is_reject, i * kTick);
    }
    EXPECT_EQ(b.total_in_window(), 100u);
    EXPECT_EQ(b.rejects_in_window(), 20u);
    EXPECT_FALSE(b.tripped()) << "20% exactly is at threshold, not above";
}

TEST(RejectRateBreakerTest, AboveThresholdTrips) {
    RejectRateBreaker b(make_cfg(/*threshold_pct=*/20.0, /*min_events=*/10));
    // 21 rejects / 100 total = 21% > 20%. All events within 10s < 60s window.
    for (int i = 0; i < 21; ++i)
        b.record(true, i * kTick);
    for (int i = 21; i < 100; ++i)
        b.record(false, i * kTick);
    EXPECT_TRUE(b.tripped());
}

TEST(RejectRateBreakerTest, LatchesAfterTrip) {
    // Once tripped, flooding the breaker with non-rejects (pushing the
    // rate well below threshold) must still report tripped() — same
    // "restart required" semantics as the daily-loss kill switch.
    RejectRateBreaker b(make_cfg(/*threshold_pct=*/20.0, /*min_events=*/10));
    for (int i = 0; i < 21; ++i)
        b.record(true, i * kTick);
    for (int i = 21; i < 100; ++i)
        b.record(false, i * kTick);
    ASSERT_TRUE(b.tripped());

    // Advance far past the window and feed only non-rejects.
    for (int i = 0; i < 500; ++i)
        b.record(false, (1000 + i) * kSec);
    EXPECT_TRUE(b.tripped()) << "Latch must not auto-clear even if window drains";
}

TEST(RejectRateBreakerTest, WindowEvictionRemovesStaleEvents) {
    // Tight 10s window. Record an initial burst of rejects well in the
    // past, then record non-rejects far enough in the future that the
    // old burst falls outside the window — counters should reflect only
    // the recent non-rejects.
    const uint64_t window_ns = 10 * kSec;
    RejectRateBreaker b(make_cfg(/*threshold_pct=*/20.0, /*min_events=*/100, window_ns));

    for (int i = 0; i < 50; ++i)
        b.record(true, i * kSec);  // 0..49s

    // Jump to 100s: eviction cutoff = 100 - 10 = 90s, so everything
    // before 90s is discarded as each new event lands.
    for (int i = 100; i < 105; ++i)
        b.record(false, i * kSec);
    EXPECT_EQ(b.total_in_window(), 5u);
    EXPECT_EQ(b.rejects_in_window(), 0u);
    EXPECT_FALSE(b.tripped());
}

TEST(RejectRateBreakerTest, NtpRegressionDoesNotUnderflow) {
    // If the first timestamp is smaller than window_ns, the naive
    // cutoff = now_ns - window_ns would underflow on uint64. The code
    // guards that — just verify it doesn't crash or trip spuriously.
    const uint64_t window_ns = 60 * kSec;
    RejectRateBreaker b(make_cfg(/*threshold_pct=*/20.0, /*min_events=*/10, window_ns));
    // Tiny timestamps well below window_ns.
    for (int i = 0; i < 15; ++i)
        b.record(/*is_reject=*/false, /*now_ns=*/static_cast<uint64_t>(i));
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.total_in_window(), 15u);
}

TEST(RejectRateBreakerTest, HighThresholdToleratesModerateRejects) {
    // threshold_pct=90 means you'd need a near-total outage to trip.
    // A 50/50 interleaved mix (one reject, one non-reject) keeps the
    // running rate at ~50% throughout, well below 90% — guards against
    // an inverted or off-by-one comparison in the trip check.
    RejectRateBreaker b(make_cfg(/*threshold_pct=*/90.0, /*min_events=*/10));
    for (int i = 0; i < 100; ++i)
        b.record(/*is_reject=*/(i % 2 == 0), i * kTick);
    EXPECT_FALSE(b.tripped());
}

}  // namespace
