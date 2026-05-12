#include "strategy/strategy/regime_classifier.h"

#include <gtest/gtest.h>
#include <cmath>

using bpt::strategy::strategy::RegimeClassifier;

namespace {

// Helper: feed a sequence of mids spaced by `interval_ns`, returning the
// final classified regime at `now_ns`.
RegimeClassifier::Regime classify_after(RegimeClassifier& cl,
                                        const std::vector<double>& mids,
                                        std::uint64_t interval_ns,
                                        std::uint64_t start_ns = 0) {
    std::uint64_t ts = start_ns;
    for (double m : mids) {
        cl.update(m, ts);
        ts += interval_ns;
    }
    return cl.classify(ts);
}

}  // namespace

// ── Quiet ────────────────────────────────────────────────────────────────────

TEST(RegimeClassifier, FlatPriceIsQuiet) {
    RegimeClassifier cl;
    std::vector<double> mids(80, 100.0);  // perfectly flat
    auto r = classify_after(cl, mids, /*interval_ns=*/1'000'000'000ULL);
    EXPECT_EQ(r, RegimeClassifier::Regime::QUIET);
    EXPECT_DOUBLE_EQ(cl.realized_vol_bps_per_min(), 0.0);
}

TEST(RegimeClassifier, NotReadyBeforeWindow) {
    RegimeClassifier cl;
    EXPECT_FALSE(cl.ready());
    cl.update(100.0, 0);
    cl.update(100.1, 1'000'000'000);
    cl.update(100.2, 2'000'000'000);
    EXPECT_FALSE(cl.ready());  // 3 samples, need ≥30 (window/2)
    // classify() returns QUIET when not ready — safer default than CHOPPY.
    EXPECT_EQ(cl.classify(3'000'000'000), RegimeClassifier::Regime::QUIET);
}

// ── Trending ─────────────────────────────────────────────────────────────────

TEST(RegimeClassifier, NoisyDriftIsTrending) {
    // Drift with realistic noise — non-zero variance and non-zero trend.
    // The classifier's purpose is to pause on CHOPPY; the contract is
    // "trending or quiet → quote, choppy → pause". Distinguishing TRENDING
    // from QUIET matters less than distinguishing both from CHOPPY.
    RegimeClassifier::Config cfg;
    cfg.window_size = 60;
    cfg.quiet_vol_bps_per_min = 0.5;
    RegimeClassifier cl(cfg);

    std::vector<double> mids;
    double p = 100.0;
    for (int i = 0; i < 80; ++i) {
        // +5 bps drift with ±1 bps noise. Trend dominates noise → TRENDING.
        const double drift = 5e-4;
        const double noise = (i % 2 == 0 ? 1e-4 : -1e-4);
        p *= (1.0 + drift + noise);
        mids.push_back(p);
    }
    auto r = classify_after(cl, mids, 1'000'000'000ULL);
    EXPECT_EQ(r, RegimeClassifier::Regime::TRENDING);
    EXPECT_GT(cl.trend_zscore(), 1.0);
}

TEST(RegimeClassifier, PerfectDriftIsNotChoppy) {
    // Edge case: perfect linear drift has zero variance, so realized vol
    // returns 0, so regime is QUIET. That's behaviourally fine — both
    // QUIET and TRENDING result in "quote normally"; only CHOPPY pauses.
    RegimeClassifier cl;
    std::vector<double> mids;
    double p = 100.0;
    for (int i = 0; i < 80; ++i) {
        p *= 1.0001;
        mids.push_back(p);
    }
    auto r = classify_after(cl, mids, 1'000'000'000ULL);
    EXPECT_NE(r, RegimeClassifier::Regime::CHOPPY);
}

// ── Choppy ──────────────────────────────────────────────────────────────────

TEST(RegimeClassifier, AlternatingReturnsAreChoppy) {
    // Equal up-and-down ticks → high vol, near-zero trend z → CHOPPY.
    RegimeClassifier cl;
    std::vector<double> mids;
    double p = 100.0;
    for (int i = 0; i < 80; ++i) {
        // Each tick: +/- 100 bps. Big enough vol to clear quiet threshold.
        p *= (i % 2 == 0) ? 1.01 : (1.0 / 1.01);
        mids.push_back(p);
    }
    auto r = classify_after(cl, mids, 1'000'000'000ULL);
    EXPECT_EQ(r, RegimeClassifier::Regime::CHOPPY);
    EXPECT_GT(cl.realized_vol_bps_per_min(), 5.0);
    EXPECT_LT(cl.trend_zscore(), 1.0);
}

// ── Hysteresis ──────────────────────────────────────────────────────────────

TEST(RegimeClassifier, ChoppyStaysSticky) {
    // After tripping CHOPPY, the regime stays CHOPPY for chop_cooldown_ns
    // even if subsequent classifications would say TRENDING.
    RegimeClassifier::Config cfg;
    cfg.chop_cooldown_ns = 60'000'000'000ULL;  // 60 s
    RegimeClassifier cl(cfg);

    // First, drive into CHOPPY.
    std::vector<double> chop;
    double p = 100.0;
    for (int i = 0; i < 80; ++i) {
        p *= (i % 2 == 0) ? 1.01 : (1.0 / 1.01);
        chop.push_back(p);
    }
    classify_after(cl, chop, 1'000'000'000ULL);
    // cooldown should be active now
    const std::uint64_t after_chop_ts = 80 * 1'000'000'000ULL;

    // Now feed a strong trending sequence; classify shortly after — should
    // still be CHOPPY due to hysteresis.
    std::uint64_t ts = after_chop_ts;
    for (int i = 0; i < 5; ++i) {
        p *= 1.0005;  // strong drift
        ts += 1'000'000'000ULL;
        cl.update(p, ts);
    }
    EXPECT_EQ(cl.classify(ts), RegimeClassifier::Regime::CHOPPY);

    // After the cooldown elapses AND the rolling window has rolled forward
    // far enough that the chop history is gone, regime can update.
    // Window size = 60 (default); push 80 trending samples so the window
    // is pure trending data.
    ts = after_chop_ts + 70'000'000'000ULL;  // start past cooldown
    for (int i = 0; i < 80; ++i) {
        p *= 1.0005;
        ts += 1'000'000'000ULL;
        cl.update(p, ts);
    }
    // Without the chop residue, the regime can flip to TRENDING (or QUIET
    // if vol fell below threshold). Either way: not CHOPPY.
    EXPECT_NE(cl.classify(ts), RegimeClassifier::Regime::CHOPPY);
}

// ── Per-minute vol units ─────────────────────────────────────────────────────

TEST(RegimeClassifier, RealizedVolMatchesExpectedScale) {
    // Inject log-returns with known stdev and check the per-minute conversion.
    // 1bps stdev per 1-second sample → sqrt(60) bps/min ≈ 7.7 bps/min.
    RegimeClassifier cl;
    double p = 100.0;
    std::uint64_t ts = 0;
    // Alternate +1/-1 bp returns — stdev = 1 bp exactly.
    for (int i = 0; i < 80; ++i) {
        p *= (i % 2 == 0) ? (1.0 + 1e-4) : (1.0 - 1e-4);
        cl.update(p, ts);
        ts += 1'000'000'000ULL;
    }
    const double rv_per_min = cl.realized_vol_bps_per_min();
    EXPECT_NEAR(rv_per_min, std::sqrt(60.0) * 1.0, 1.0);  // ~7.7, within 1 bp slack
}

// ── Reset ────────────────────────────────────────────────────────────────────

TEST(RegimeClassifier, ResetClearsState) {
    RegimeClassifier cl;
    std::vector<double> mids(80, 100.0);
    classify_after(cl, mids, 1'000'000'000ULL);
    EXPECT_TRUE(cl.ready());
    cl.reset();
    EXPECT_FALSE(cl.ready());
    EXPECT_DOUBLE_EQ(cl.realized_vol_bps_per_min(), 0.0);
}
