// Unit tests for DisconnectRateBreaker.

#include "order_gateway/risk/disconnect_rate_breaker.h"

#include <gtest/gtest.h>

namespace {

using bpt::order_gateway::risk::DisconnectRateBreaker;

constexpr uint64_t kSec = 1'000'000'000ULL;
constexpr uint64_t kTick = 100'000'000ULL;  // 100ms

DisconnectRateBreaker::Config make_cfg(uint32_t threshold = 5, uint64_t window_ns = 60 * kSec) {
    DisconnectRateBreaker::Config c;
    c.enabled = true;
    c.threshold = threshold;
    c.window_ns = window_ns;
    return c;
}

TEST(DisconnectRateBreakerTest, DisabledRecordIsNoOp) {
    DisconnectRateBreaker b({});  // enabled=false
    for (int i = 0; i < 100; ++i)
        b.record(i * kSec);
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.count_in_window(), 0u);
}

TEST(DisconnectRateBreakerTest, BelowThresholdDoesNotTrip) {
    DisconnectRateBreaker b(make_cfg(/*threshold=*/5));
    for (int i = 0; i < 4; ++i)
        b.record(i * kTick);
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.count_in_window(), 4u);
}

TEST(DisconnectRateBreakerTest, AtThresholdTrips) {
    // Trip condition is count >= threshold (inclusive). A rapid burst
    // of exactly `threshold` disconnects in-window should latch — this
    // is the most common real signal (reconnect loop).
    DisconnectRateBreaker b(make_cfg(/*threshold=*/5));
    for (int i = 0; i < 5; ++i)
        b.record(i * kTick);
    EXPECT_TRUE(b.tripped());
}

TEST(DisconnectRateBreakerTest, LatchesEvenAfterWindowDrains) {
    DisconnectRateBreaker b(make_cfg(/*threshold=*/5, /*window_ns=*/10 * kSec));
    for (int i = 0; i < 5; ++i)
        b.record(i * kTick);
    ASSERT_TRUE(b.tripped());

    // Jump far outside window; feed one more event → window now has
    // just that single event, well below threshold, but latch persists.
    b.record(1000 * kSec);
    EXPECT_EQ(b.count_in_window(), 1u);
    EXPECT_TRUE(b.tripped());
}

TEST(DisconnectRateBreakerTest, SparseDisconnectsDoNotTrip) {
    // Threshold=5, window=60s. Space events 20s apart → window sees
    // at most 4 concurrently (cutoff = now-60, events with ts>=cutoff
    // retained). Simulates healthy brief blips that stay below latch.
    DisconnectRateBreaker b(make_cfg(/*threshold=*/5, /*window_ns=*/60 * kSec));
    for (int i = 0; i < 10; ++i)
        b.record(i * 20 * kSec);
    EXPECT_FALSE(b.tripped());
    EXPECT_LE(b.count_in_window(), 4u) << "60s window spaced at 20s should hold ≤ 4 events";
}

TEST(DisconnectRateBreakerTest, NtpRegressionDoesNotUnderflow) {
    DisconnectRateBreaker b(make_cfg(/*threshold=*/5, /*window_ns=*/60 * kSec));
    for (int i = 0; i < 4; ++i)
        b.record(static_cast<uint64_t>(i));
    EXPECT_FALSE(b.tripped());
    EXPECT_EQ(b.count_in_window(), 4u);
}

}  // namespace
