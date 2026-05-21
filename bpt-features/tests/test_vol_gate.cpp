#include "features/vol_gate.h"

#include <gtest/gtest.h>

namespace {

using bpt::features::VolatilityGate;

constexpr uint64_t SEC = 1'000'000'000ULL;

// ---------------------------------------------------------------------------
// Disabled gate
// ---------------------------------------------------------------------------

TEST(VolatilityGateTest, DisabledByDefault) {
    VolatilityGate gate;
    EXPECT_FALSE(gate.enabled());
    EXPECT_FALSE(gate.update_and_check(100.0, 1 * SEC));
    EXPECT_FALSE(gate.is_halted(2 * SEC));
}

TEST(VolatilityGateTest, DisabledWhenMaxBpsZero) {
    VolatilityGate gate({.max_bps_per_window = 0.0, .window_ns = SEC, .halt_duration_ns = 5 * SEC});
    EXPECT_FALSE(gate.enabled());
    // Even a huge move should not trip
    EXPECT_FALSE(gate.update_and_check(100.0, 1 * SEC));
    EXPECT_FALSE(gate.update_and_check(200.0, 1 * SEC + 1));
}

// ---------------------------------------------------------------------------
// Basic trip and recovery
// ---------------------------------------------------------------------------

TEST(VolatilityGateTest, TripsOnLargeMove) {
    // 10 bps threshold, 1s window, 5s halt
    VolatilityGate gate({.max_bps_per_window = 10.0, .window_ns = SEC, .halt_duration_ns = 5 * SEC});
    EXPECT_TRUE(gate.enabled());

    // Stable mid — no trip
    EXPECT_FALSE(gate.update_and_check(100.0, 1 * SEC));
    EXPECT_FALSE(gate.update_and_check(100.0, 1 * SEC + 100'000'000));

    // 20 bps move up — should trip
    // 100 * (1 + 20/10000) = 100.20
    bool halted = gate.update_and_check(100.20, 1 * SEC + 200'000'000);
    EXPECT_TRUE(halted);
    EXPECT_EQ(gate.trips_total(), 1u);
    EXPECT_GT(gate.last_trip_bps(), 10.0);
}

TEST(VolatilityGateTest, HaltExpiresAfterDuration) {
    VolatilityGate gate({.max_bps_per_window = 10.0, .window_ns = SEC, .halt_duration_ns = 5 * SEC});

    gate.update_and_check(100.0, 1 * SEC);
    gate.update_and_check(100.20, 1 * SEC + 100'000'000);  // trip

    // Still halted at +3s
    EXPECT_TRUE(gate.is_halted(1 * SEC + 3 * SEC));

    // Not halted at +6s (halt duration = 5s, trip was at ~1.1s)
    EXPECT_FALSE(gate.is_halted(1 * SEC + 100'000'000 + 6 * SEC));
}

TEST(VolatilityGateTest, ReTripsAfterExpiry) {
    VolatilityGate gate({.max_bps_per_window = 10.0, .window_ns = SEC, .halt_duration_ns = 2 * SEC});

    // First trip
    gate.update_and_check(100.0, 1 * SEC);
    gate.update_and_check(100.20, 1 * SEC + 100'000'000);
    EXPECT_EQ(gate.trips_total(), 1u);

    // Halt expires, new window, another spike
    gate.update_and_check(100.20, 10 * SEC);
    bool halted = gate.update_and_check(100.40, 10 * SEC + 100'000'000);
    EXPECT_TRUE(halted);
    EXPECT_EQ(gate.trips_total(), 2u);
}

// ---------------------------------------------------------------------------
// Window eviction
// ---------------------------------------------------------------------------

TEST(VolatilityGateTest, OldSamplesEvicted) {
    // 10 bps threshold, 1s window
    VolatilityGate gate({.max_bps_per_window = 10.0, .window_ns = SEC, .halt_duration_ns = 5 * SEC});

    // Price at t=0
    gate.update_and_check(100.0, 0);
    // Price at t=0.5s — small move
    EXPECT_FALSE(gate.update_and_check(100.05, 500'000'000));
    // Price at t=1.5s — the t=0 sample is now outside the 1s window.
    // Only the t=0.5s sample remains. Move from 100.05→100.08 is ~3 bps, no trip.
    EXPECT_FALSE(gate.update_and_check(100.08, 1'500'000'000));
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(VolatilityGateTest, SingleSampleNeverTrips) {
    VolatilityGate gate({.max_bps_per_window = 1.0, .window_ns = SEC, .halt_duration_ns = SEC});
    // First update — no history to compare against
    EXPECT_FALSE(gate.update_and_check(100.0, 1 * SEC));
    // is_halted also false
    EXPECT_FALSE(gate.is_halted(1 * SEC));
}

TEST(VolatilityGateTest, NegativeMidIgnored) {
    VolatilityGate gate({.max_bps_per_window = 10.0, .window_ns = SEC, .halt_duration_ns = SEC});
    EXPECT_FALSE(gate.update_and_check(-1.0, 1 * SEC));
    EXPECT_FALSE(gate.update_and_check(0.0, 2 * SEC));
}

TEST(VolatilityGateTest, DownwardMoveTrips) {
    VolatilityGate gate({.max_bps_per_window = 10.0, .window_ns = SEC, .halt_duration_ns = 5 * SEC});

    gate.update_and_check(100.0, 1 * SEC);
    // 20 bps move DOWN
    bool halted = gate.update_and_check(99.80, 1 * SEC + 100'000'000);
    EXPECT_TRUE(halted);
}

TEST(VolatilityGateTest, ExactThresholdDoesNotTrip) {
    // max_bps = 10, move is exactly 10 bps → condition is strictly greater,
    // so it should NOT trip.
    VolatilityGate gate({.max_bps_per_window = 10.0, .window_ns = SEC, .halt_duration_ns = 5 * SEC});

    gate.update_and_check(100.0, 1 * SEC);
    // Exactly 10 bps: (100 - 100.10) / 100.10 * 10000 ≈ 9.99 (denom is current mid)
    // Let's compute: we need |prev - curr| / curr * 1e4 > 10.
    // |100.0 - 100.10| / 100.10 * 10000 = 9.99... < 10 → no trip
    bool halted = gate.update_and_check(100.10, 1 * SEC + 100'000'000);
    EXPECT_FALSE(halted);
}

}  // namespace
