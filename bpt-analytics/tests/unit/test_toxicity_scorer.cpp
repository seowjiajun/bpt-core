#include "analytics/analysis/toxicity_scorer.h"

#include <cmath>
#include <gtest/gtest.h>

namespace {

using bpt::analytics::analysis::MarkoutTracker;
using bpt::analytics::analysis::ToxicityScorer;

constexpr uint64_t SEC = 1'000'000'000ULL;
constexpr uint64_t INST = 100;

MarkoutTracker::Observation make_obs(int side_sign, double markout_5s, uint64_t ts = 0) {
    return {
        .instrument_id = INST,
        .fill_ts_ns = ts,
        .side_sign = side_sign,
        .fill_price = 75000.0,
        .fill_mid = 75000.0,
        .markout_1s_bps = markout_5s * 0.3,  // doesn't matter for scoring
        .markout_5s_bps = markout_5s,
        .markout_30s_bps = markout_5s * 1.5,
    };
}

// ---------------------------------------------------------------------------
// Insufficient samples
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, InsufficientSamplesReturnsNaN) {
    ToxicityScorer scorer({.window_size = 50, .min_samples = 5});
    // Only 2 fills — below min_samples
    scorer.add(make_obs(+1, 1.0));
    scorer.add(make_obs(+1, -1.0));

    auto update = scorer.compute(INST, 100 * SEC);
    EXPECT_TRUE(std::isnan(update.bid_markout_5s_bps));
    EXPECT_TRUE(std::isnan(update.bid_adverse_rate));
    EXPECT_EQ(update.bid_sample_count, 2u);
}

// ---------------------------------------------------------------------------
// All favorable fills
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, AllFavorable) {
    ToxicityScorer scorer({.window_size = 50, .min_samples = 3});
    // 5 buy fills, all positive markout (favorable)
    for (int i = 0; i < 5; ++i)
        scorer.add(make_obs(+1, 2.0, i * SEC));

    auto update = scorer.compute(INST, 100 * SEC);
    EXPECT_NEAR(update.bid_markout_5s_bps, 2.0, 0.01);
    EXPECT_NEAR(update.bid_adverse_rate, 0.0, 0.01);
    EXPECT_GT(update.bid_toxicity_score, 0.0);  // positive = not toxic
    EXPECT_EQ(update.bid_sample_count, 5u);
}

// ---------------------------------------------------------------------------
// All adverse fills
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, AllAdverse) {
    ToxicityScorer scorer({.window_size = 50, .min_samples = 3});
    // 5 sell fills, all negative markout (adverse — price went up after we sold)
    for (int i = 0; i < 5; ++i)
        scorer.add(make_obs(-1, -3.0, i * SEC));

    auto update = scorer.compute(INST, 100 * SEC);
    EXPECT_NEAR(update.ask_markout_5s_bps, -3.0, 0.01);
    EXPECT_NEAR(update.ask_adverse_rate, 1.0, 0.01);
    EXPECT_LT(update.ask_toxicity_score, 0.0);  // negative = toxic
    EXPECT_EQ(update.ask_sample_count, 5u);
}

// ---------------------------------------------------------------------------
// Mixed fills
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, MixedFills) {
    ToxicityScorer scorer({.window_size = 50, .min_samples = 3});
    // 3 favorable, 2 adverse on bid side
    scorer.add(make_obs(+1, 2.0, 1 * SEC));
    scorer.add(make_obs(+1, 3.0, 2 * SEC));
    scorer.add(make_obs(+1, -1.0, 3 * SEC));
    scorer.add(make_obs(+1, 1.0, 4 * SEC));
    scorer.add(make_obs(+1, -2.0, 5 * SEC));

    auto update = scorer.compute(INST, 100 * SEC);
    // Mean: (2+3-1+1-2)/5 = 0.6
    EXPECT_NEAR(update.bid_markout_5s_bps, 0.6, 0.01);
    // Adverse rate: 2/5 = 0.4
    EXPECT_NEAR(update.bid_adverse_rate, 0.4, 0.01);
    EXPECT_EQ(update.bid_sample_count, 5u);
}

// ---------------------------------------------------------------------------
// Per-side independence
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, SidesAreIndependent) {
    ToxicityScorer scorer({.window_size = 50, .min_samples = 3});
    // Buys are favorable
    for (int i = 0; i < 5; ++i)
        scorer.add(make_obs(+1, 2.0, i * SEC));
    // Sells are adverse
    for (int i = 0; i < 5; ++i)
        scorer.add(make_obs(-1, -3.0, (i + 5) * SEC));

    auto update = scorer.compute(INST, 100 * SEC);
    EXPECT_GT(update.bid_markout_5s_bps, 0.0);  // favorable
    EXPECT_LT(update.ask_markout_5s_bps, 0.0);  // adverse
}

// ---------------------------------------------------------------------------
// Window eviction by size
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, WindowEvictsBySize) {
    ToxicityScorer scorer({.window_size = 3, .min_samples = 1});

    scorer.add(make_obs(+1, -10.0, 1 * SEC));  // will be evicted
    scorer.add(make_obs(+1, 2.0, 2 * SEC));
    scorer.add(make_obs(+1, 2.0, 3 * SEC));
    scorer.add(make_obs(+1, 2.0, 4 * SEC));  // evicts the -10

    auto update = scorer.compute(INST, 100 * SEC);
    // Should only reflect the last 3: all +2.0
    EXPECT_NEAR(update.bid_markout_5s_bps, 2.0, 0.01);
    EXPECT_EQ(update.bid_sample_count, 3u);
}

// ---------------------------------------------------------------------------
// Window eviction by duration
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, WindowEvictsByDuration) {
    ToxicityScorer scorer({.window_size = 100, .window_duration_ns = 10 * SEC, .min_samples = 1});

    scorer.add(make_obs(+1, -10.0, 1 * SEC));  // will expire (12 - 1 > 10)
    scorer.add(make_obs(+1, 2.0, 9 * SEC));
    scorer.add(make_obs(+1, 2.0, 10 * SEC));
    scorer.add(make_obs(+1, 2.0, 12 * SEC));  // triggers eviction of t=1

    auto update = scorer.compute(INST, 100 * SEC);
    EXPECT_NEAR(update.bid_markout_5s_bps, 2.0, 0.01);
    EXPECT_EQ(update.bid_sample_count, 3u);
}

// ---------------------------------------------------------------------------
// Toxicity score weighting
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, ToxicityScoreWeightedByAdverseRate) {
    // Same mean markout, different adverse rates → different scores
    ToxicityScorer scorer1({.window_size = 50, .min_samples = 1});
    ToxicityScorer scorer2({.window_size = 50, .min_samples = 1});

    // Scorer1: 2 adverse out of 5
    scorer1.add(make_obs(-1, -5.0, 1 * SEC));
    scorer1.add(make_obs(-1, -5.0, 2 * SEC));
    scorer1.add(make_obs(-1, 5.0, 3 * SEC));
    scorer1.add(make_obs(-1, 5.0, 4 * SEC));
    scorer1.add(make_obs(-1, 5.0, 5 * SEC));

    // Scorer2: 4 adverse out of 5
    scorer2.add(make_obs(-1, -5.0, 1 * SEC));
    scorer2.add(make_obs(-1, -5.0, 2 * SEC));
    scorer2.add(make_obs(-1, -5.0, 3 * SEC));
    scorer2.add(make_obs(-1, -5.0, 4 * SEC));
    scorer2.add(make_obs(-1, 9.0, 5 * SEC));

    auto u1 = scorer1.compute(INST, 100 * SEC);
    auto u2 = scorer2.compute(INST, 100 * SEC);

    // Scorer2 has higher adverse rate → more negative toxicity score
    EXPECT_LT(u2.ask_toxicity_score, u1.ask_toxicity_score);
}

// ---------------------------------------------------------------------------
// Empty scorer
// ---------------------------------------------------------------------------

TEST(ToxicityScorerTest, EmptyScorerReturnsNaN) {
    ToxicityScorer scorer;
    auto update = scorer.compute(INST, 100 * SEC);
    EXPECT_TRUE(std::isnan(update.bid_markout_5s_bps));
    EXPECT_TRUE(std::isnan(update.ask_markout_5s_bps));
    EXPECT_EQ(update.bid_sample_count, 0u);
    EXPECT_EQ(update.ask_sample_count, 0u);
}

}  // namespace
