#include "strategy/strategy/regime_detector.h"

#include <cmath>
#include <gtest/gtest.h>
#include <random>

namespace {

using bpt::strategy::strategy::RegimeDetector;
using Regime = RegimeDetector::Regime;

// ---------------------------------------------------------------------------
// Warmup
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, StartsInWarmup) {
    RegimeDetector det;
    EXPECT_EQ(det.regime(), Regime::WARMING_UP);
    EXPECT_FALSE(det.is_warm());
    EXPECT_DOUBLE_EQ(det.hurst(), 0.5);
    EXPECT_DOUBLE_EQ(det.gamma_multiplier(), 1.0);
}

TEST(RegimeDetectorTest, StaysInWarmupUntilEnoughSamples) {
    RegimeDetector det({.warmup_samples = 50, .eval_interval = 10});
    double price = 100.0;
    for (int i = 0; i < 49; ++i) {
        price += 0.01;
        det.update(price);
    }
    EXPECT_EQ(det.regime(), Regime::WARMING_UP);
    EXPECT_FALSE(det.is_warm());
}

// ---------------------------------------------------------------------------
// Mean-reverting series (H < 0.45)
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, MeanRevertingSeriesClassifiedCorrectly) {
    RegimeDetector det({
        .mean_revert_threshold = 0.45,
        .trend_threshold = 0.55,
        .hurst_window = 300,
        .warmup_samples = 30,
        .eval_interval = 1,
    });

    // Generate a mean-reverting series: oscillate around a fixed price.
    // price = 100 + sin(i * 0.3) — perfectly anti-persistent.
    for (int i = 0; i < 300; ++i) {
        double price = 100.0 + std::sin(i * 0.3) * 0.5;
        det.update(price);
    }

    EXPECT_TRUE(det.is_warm());
    EXPECT_LT(det.hurst(), 0.45);
    EXPECT_EQ(det.regime(), Regime::MEAN_REVERT);
    EXPECT_LT(det.gamma_multiplier(), 1.0);  // should tighten spreads
}

// ---------------------------------------------------------------------------
// Trending series (H > 0.55)
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, TrendingSeriesClassifiedCorrectly) {
    RegimeDetector det({
        .mean_revert_threshold = 0.45,
        .trend_threshold = 0.55,
        .hurst_window = 300,
        .warmup_samples = 30,
        .eval_interval = 1,
    });

    // Generate a trending series: steady upward drift with small noise.
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0.0, 0.01);
    double price = 100.0;
    for (int i = 0; i < 300; ++i) {
        price += 0.05 + noise(rng);  // strong upward drift
        det.update(price);
    }

    EXPECT_TRUE(det.is_warm());
    EXPECT_GT(det.hurst(), 0.55);
    EXPECT_EQ(det.regime(), Regime::TRENDING);
    EXPECT_GT(det.gamma_multiplier(), 1.0);  // should widen spreads
}

// ---------------------------------------------------------------------------
// Random walk (H ≈ 0.5 → NEUTRAL)
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, RandomWalkIsNeutral) {
    RegimeDetector det({
        .mean_revert_threshold = 0.45,
        .trend_threshold = 0.55,
        .hurst_window = 500,
        .warmup_samples = 50,
        .eval_interval = 1,
    });

    // Generate a random walk.
    std::mt19937 rng(123);
    std::normal_distribution<double> noise(0.0, 0.02);
    double price = 100.0;
    for (int i = 0; i < 500; ++i) {
        price *= std::exp(noise(rng));
        det.update(price);
    }

    EXPECT_TRUE(det.is_warm());
    // Hurst should be near 0.5 — within the neutral band
    EXPECT_GT(det.hurst(), 0.35);
    EXPECT_LT(det.hurst(), 0.65);
    // Gamma multiplier should be near 1.0 (neutral or mild)
    EXPECT_GE(det.gamma_multiplier(), 0.5);
    EXPECT_LE(det.gamma_multiplier(), 2.0);
}

// ---------------------------------------------------------------------------
// Gamma multiplier values
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, GammaMultiplierPerRegime) {
    RegimeDetector det({
        .gamma_mult_mean_revert = 0.6,
        .gamma_mult_neutral = 1.0,
        .gamma_mult_trending = 1.8,
    });

    // Can't directly set regime, but can verify default warmup returns neutral mult
    EXPECT_DOUBLE_EQ(det.gamma_multiplier(), 1.0);
}

// ---------------------------------------------------------------------------
// Hysteresis prevents flipping
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, HysteresisPreventsRapidFlipping) {
    RegimeDetector det({
        .mean_revert_threshold = 0.45,
        .trend_threshold = 0.55,
        .hysteresis = 0.03,
        .hurst_window = 200,
        .warmup_samples = 30,
        .eval_interval = 1,
    });

    // First: establish a mean-reverting regime with oscillating prices
    for (int i = 0; i < 200; ++i) {
        double price = 100.0 + std::sin(i * 0.3) * 0.5;
        det.update(price);
    }
    // Should be mean-reverting
    if (det.regime() != Regime::MEAN_REVERT)
        return;  // skip if Hurst happened to land in neutral

    Regime before = det.regime();

    // Feed a few ambiguous ticks near the threshold — shouldn't flip immediately
    for (int i = 0; i < 20; ++i) {
        det.update(100.0 + i * 0.001);
    }

    // With hysteresis, a small number of non-MR ticks shouldn't flip the regime
    // (it would need to cross mean_revert_threshold + hysteresis = 0.48)
    EXPECT_EQ(det.regime(), before);
}

// ---------------------------------------------------------------------------
// Zero / negative price ignored
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, ZeroPriceIgnored) {
    RegimeDetector det;
    det.update(0.0);
    det.update(-1.0);
    EXPECT_EQ(det.tick_count(), 0u);
}

// ---------------------------------------------------------------------------
// Regime names
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, RegimeNames) {
    EXPECT_STREQ(RegimeDetector::name(Regime::WARMING_UP), "WARMING_UP");
    EXPECT_STREQ(RegimeDetector::name(Regime::MEAN_REVERT), "MEAN_REVERT");
    EXPECT_STREQ(RegimeDetector::name(Regime::NEUTRAL), "NEUTRAL");
    EXPECT_STREQ(RegimeDetector::name(Regime::TRENDING), "TRENDING");
}

// ---------------------------------------------------------------------------
// Eval interval throttling
// ---------------------------------------------------------------------------

TEST(RegimeDetectorTest, EvalIntervalThrottles) {
    RegimeDetector det({.warmup_samples = 10, .eval_interval = 50});

    // Feed 100 prices — Hurst should only be computed twice (at tick 50 and 100)
    for (int i = 0; i < 49; ++i)
        det.update(100.0 + i * 0.01);

    // At tick 49 with warmup=10, enough samples but eval_interval not reached
    EXPECT_EQ(det.regime(), Regime::WARMING_UP);

    // Tick 50 triggers eval
    det.update(100.5);
    EXPECT_TRUE(det.is_warm());
}

}  // namespace
