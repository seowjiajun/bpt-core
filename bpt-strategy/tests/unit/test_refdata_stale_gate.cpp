#include "strategy/strategy/refdata_stale_gate.h"

#include <gtest/gtest.h>

namespace {

using bpt::strategy::strategy::RefdataStaleGate;
using State = RefdataStaleGate::State;

constexpr uint64_t SEC = 1'000'000'000ULL;

// ---------------------------------------------------------------------------
// Default behaviour
// ---------------------------------------------------------------------------

TEST(RefdataStaleGateTest, DefaultsAreNotStale) {
    RefdataStaleGate gate;
    EXPECT_FALSE(gate.is_stale());
}

// Without started_at_ns, never receiving a heartbeat is Ok forever — caller
// is responsible for anchoring the startup window.
TEST(RefdataStaleGateTest, NoStartupAnchorMeansNoStartupTimeout) {
    RefdataStaleGate gate;
    EXPECT_EQ(gate.evaluate(/*now=*/1000 * SEC, /*hb=*/0), State::Ok);
}

// ---------------------------------------------------------------------------
// Startup phase — never received a heartbeat
// ---------------------------------------------------------------------------

// In real usage, set_started_at takes a steady_clock nanos-since-epoch
// value (huge), never 0 — 0 is the gate's sentinel for "no anchor set"
// (see NoStartupAnchorMeansNoStartupTimeout above). Use a non-zero
// anchor in startup-phase tests.
constexpr uint64_t T0 = 1'000'000 * SEC;  // arbitrary non-zero anchor

TEST(RefdataStaleGateTest, StartupOkBeforeTimeout) {
    RefdataStaleGate gate({.startup_timeout_ns = 60 * SEC, .stale_threshold_ns = 25 * SEC});
    gate.set_started_at(T0);

    EXPECT_EQ(gate.evaluate(T0, 0), State::Ok);
    EXPECT_EQ(gate.evaluate(T0 + 30 * SEC, 0), State::Ok);
    EXPECT_EQ(gate.evaluate(T0 + 59 * SEC, 0), State::Ok);
}

TEST(RefdataStaleGateTest, StartupTimedOutAfterThreshold) {
    RefdataStaleGate gate({.startup_timeout_ns = 60 * SEC, .stale_threshold_ns = 25 * SEC});
    gate.set_started_at(T0);

    // Just past the threshold — should fire StartupTimedOut.
    EXPECT_EQ(gate.evaluate(T0 + 61 * SEC, 0), State::StartupTimedOut);
    // Continues firing every call until a heartbeat arrives.
    EXPECT_EQ(gate.evaluate(T0 + 120 * SEC, 0), State::StartupTimedOut);
}

TEST(RefdataStaleGateTest, FirstHeartbeatClearsStartupConcern) {
    RefdataStaleGate gate({.startup_timeout_ns = 60 * SEC, .stale_threshold_ns = 25 * SEC});
    gate.set_started_at(T0);

    // Heartbeat arrives at t=10s. From then on, startup_timeout no longer applies.
    const uint64_t hb = T0 + 10 * SEC;
    EXPECT_EQ(gate.evaluate(T0 + 10 * SEC, hb), State::Ok);
    EXPECT_EQ(gate.evaluate(T0 + 20 * SEC, hb), State::Ok);
    // Even past 60s wall clock, a recent heartbeat (within stale threshold) is fine.
    EXPECT_EQ(gate.evaluate(T0 + 70 * SEC, T0 + 50 * SEC), State::Ok);
}

// ---------------------------------------------------------------------------
// Steady-state — heartbeat already received
// ---------------------------------------------------------------------------

TEST(RefdataStaleGateTest, FreshHeartbeatIsOk) {
    RefdataStaleGate gate({.stale_threshold_ns = 25 * SEC});
    EXPECT_EQ(gate.evaluate(100 * SEC, 95 * SEC), State::Ok);  // 5s old, < 25s
}

TEST(RefdataStaleGateTest, GoneStaleFiresOnceOnEdge) {
    RefdataStaleGate gate({.stale_threshold_ns = 25 * SEC});

    // 30s gap → stale. First evaluate returns GoneStale.
    EXPECT_EQ(gate.evaluate(100 * SEC, 70 * SEC), State::GoneStale);
    EXPECT_TRUE(gate.is_stale());
    // Subsequent calls with continued staleness return GoneStale again
    // (but is_stale stays true, so caller checks edge themselves).
    EXPECT_EQ(gate.evaluate(110 * SEC, 70 * SEC), State::GoneStale);
}

TEST(RefdataStaleGateTest, RecoveredFiresOnEdgeFromStale) {
    RefdataStaleGate gate({.stale_threshold_ns = 25 * SEC});

    // Trip the stale flag.
    EXPECT_EQ(gate.evaluate(100 * SEC, 70 * SEC), State::GoneStale);
    EXPECT_TRUE(gate.is_stale());

    // Fresh heartbeat arrives at t=110s. Next evaluate flips to Recovered.
    EXPECT_EQ(gate.evaluate(111 * SEC, 110 * SEC), State::Recovered);
    EXPECT_FALSE(gate.is_stale());

    // Subsequent calls return Ok, not Recovered (edge already consumed).
    EXPECT_EQ(gate.evaluate(112 * SEC, 110 * SEC), State::Ok);
}

TEST(RefdataStaleGateTest, ThresholdBoundariesInclusive) {
    RefdataStaleGate gate({.stale_threshold_ns = 25 * SEC});

    // Exactly at threshold → still Ok (the gate uses strict >, not >=).
    EXPECT_EQ(gate.evaluate(125 * SEC, 100 * SEC), State::Ok);
    // One ns past threshold → GoneStale.
    EXPECT_EQ(gate.evaluate(125 * SEC + 1, 100 * SEC), State::GoneStale);
}

TEST(RefdataStaleGateTest, FlapResistance) {
    // Verify that crossing the threshold once and then immediately recovering
    // produces a clean GoneStale → Recovered → Ok sequence with no spurious
    // re-edges. This is the property StrategyService depends on for its
    // log/metric edge-trigger semantics.
    RefdataStaleGate gate({.stale_threshold_ns = 25 * SEC});

    EXPECT_EQ(gate.evaluate(100 * SEC, 70 * SEC), State::GoneStale);
    EXPECT_EQ(gate.evaluate(101 * SEC, 70 * SEC), State::GoneStale);   // still stale
    EXPECT_EQ(gate.evaluate(101 * SEC, 100 * SEC), State::Recovered);  // fresh hb
    EXPECT_EQ(gate.evaluate(102 * SEC, 100 * SEC), State::Ok);
    EXPECT_EQ(gate.evaluate(103 * SEC, 100 * SEC), State::Ok);
}

TEST(RefdataStaleGateTest, FutureHeartbeatDoesNotUnderflow) {
    // bpt-refdata publishes its heartbeat with its own TscClock instance;
    // small cross-process calibration skew can put hb a few ms ahead of
    // strategy's now_ns. Without the guard, (now_ns - hb) wraps uint64
    // and trivially exceeds the threshold, flapping the gate on every
    // heartbeat. Verified live 2026-05-08 — pre-fix log showed
    // "Refdata heartbeat stale (18446744073.X s, threshold=25.0s)".
    RefdataStaleGate gate({.stale_threshold_ns = 25 * SEC});

    // hb arrives 1ms ahead of local clock — must NOT trip stale.
    EXPECT_EQ(gate.evaluate(100 * SEC, 100 * SEC + 1'000'000), State::Ok);
    EXPECT_FALSE(gate.is_stale());

    // Far-future hb (1 hour ahead) — also must not trip stale.
    EXPECT_EQ(gate.evaluate(100 * SEC, 100 * SEC + 3600 * SEC), State::Ok);
    EXPECT_FALSE(gate.is_stale());
}

TEST(RefdataStaleGateTest, FutureHeartbeatDuringStaleClearsStale) {
    // If we're already in a stale episode and a future-dated heartbeat
    // arrives, treat it like any other fresh heartbeat: flip Recovered.
    RefdataStaleGate gate({.stale_threshold_ns = 25 * SEC});

    EXPECT_EQ(gate.evaluate(100 * SEC, 70 * SEC), State::GoneStale);
    EXPECT_TRUE(gate.is_stale());
    // Future-dated hb (skewed clock between processes) — should recover.
    EXPECT_EQ(gate.evaluate(101 * SEC, 102 * SEC), State::Recovered);
    EXPECT_FALSE(gate.is_stale());
    EXPECT_EQ(gate.evaluate(103 * SEC, 102 * SEC), State::Ok);
}

}  // namespace
