#include "features/fair_value.h"
#include "bpt_common/book/order_book_state.h"

#include <cmath>
#include <gtest/gtest.h>

namespace {

using bpt::features::FairValueEstimator;
using bpt::common::book::OrderBookState;
using Mode = FairValueEstimator::Mode;
using Level = OrderBookState::Level;

// Builds a one-level book; convenience for the common case.
OrderBookState make_book(double bid_px, double bid_qty, double ask_px, double ask_qty) {
    OrderBookState b;
    b.apply({{bid_px, bid_qty}}, {{ask_px, ask_qty}}, 1, 1000);
    return b;
}

// ---------------------------------------------------------------------------
// Unready book: NaN propagation
// ---------------------------------------------------------------------------

TEST(FairValueEstimatorTest, UnreadyBookReturnsNaN) {
    FairValueEstimator fv{{.mode = Mode::kMid}};
    OrderBookState empty;
    EXPECT_TRUE(std::isnan(fv.estimate(empty)));
    EXPECT_TRUE(std::isnan(fv.last_estimate()));
}

TEST(FairValueEstimatorTest, RecoversFromUnreadyToReady) {
    FairValueEstimator fv{{.mode = Mode::kMicro}};
    OrderBookState empty;
    EXPECT_TRUE(std::isnan(fv.estimate(empty)));

    auto book = make_book(100.0, 10.0, 102.0, 10.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
    EXPECT_DOUBLE_EQ(fv.last_estimate(), 101.0);
}

// ---------------------------------------------------------------------------
// Mid mode
// ---------------------------------------------------------------------------

TEST(FairValueEstimatorTest, MidModeReturnsMidpoint) {
    FairValueEstimator fv{{.mode = Mode::kMid}};
    auto book = make_book(100.0, 10.0, 102.0, 10.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
}

TEST(FairValueEstimatorTest, MidModeIgnoresQuantityImbalance) {
    FairValueEstimator fv{{.mode = Mode::kMid}};
    // Mid is naive — same answer regardless of qty asymmetry.
    auto a = make_book(100.0, 1.0, 102.0, 1000.0);
    auto b = make_book(100.0, 1000.0, 102.0, 1.0);
    EXPECT_DOUBLE_EQ(fv.estimate(a), 101.0);
    EXPECT_DOUBLE_EQ(fv.estimate(b), 101.0);
}

// ---------------------------------------------------------------------------
// Micro mode
// ---------------------------------------------------------------------------

TEST(FairValueEstimatorTest, MicroBalancedQtyEqualsMid) {
    FairValueEstimator fv{{.mode = Mode::kMicro}};
    auto book = make_book(100.0, 10.0, 102.0, 10.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
}

TEST(FairValueEstimatorTest, MicroTiltsTowardThinSide) {
    // bid_qty=20, ask_qty=5. Heavier bid side → upward pressure (sweep
    // the thin ask) → micro should sit ABOVE mid, toward the ask price.
    // Counterintuitive at first; this test pins the sign.
    FairValueEstimator fv{{.mode = Mode::kMicro}};
    auto book = make_book(100.0, 20.0, 102.0, 5.0);
    // micro = (100*5 + 102*20) / (20+5) = 2540 / 25 = 101.6
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.6);
    EXPECT_GT(fv.last_estimate(), 101.0);  // above mid
}

TEST(FairValueEstimatorTest, MicroTiltsTowardThinSideOther) {
    // Mirror: heavier ask side → downward pressure → micro below mid.
    FairValueEstimator fv{{.mode = Mode::kMicro}};
    auto book = make_book(100.0, 5.0, 102.0, 20.0);
    // micro = (100*20 + 102*5) / 25 = 2510 / 25 = 100.4
    EXPECT_DOUBLE_EQ(fv.estimate(book), 100.4);
    EXPECT_LT(fv.last_estimate(), 101.0);  // below mid
}

// ---------------------------------------------------------------------------
// Micro size-capped
// ---------------------------------------------------------------------------

TEST(FairValueEstimatorTest, MicroCappedWithZeroCapBehavesLikeUncapped) {
    // size_cap_qty=0 documents "no cap" — must NOT clamp to zero.
    FairValueEstimator fv{{.mode = Mode::kMicroSizeCapped, .size_cap_qty = 0.0}};
    auto book = make_book(100.0, 20.0, 102.0, 5.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.6);
}

TEST(FairValueEstimatorTest, MicroCappedClampsBigSide) {
    // bid_qty=20 capped to 10, ask_qty=5 untouched.
    // micro' = (100*5 + 102*10) / (10+5) = 1520 / 15 ≈ 101.333
    FairValueEstimator fv{{.mode = Mode::kMicroSizeCapped, .size_cap_qty = 10.0}};
    auto book = make_book(100.0, 20.0, 102.0, 5.0);
    EXPECT_NEAR(fv.estimate(book), 101.333333, 1e-6);
    // Result should be *between* mid (101.0) and uncapped micro (101.6).
    EXPECT_GT(fv.last_estimate(), 101.0);
    EXPECT_LT(fv.last_estimate(), 101.6);
}

TEST(FairValueEstimatorTest, MicroCappedNoEffectWhenBelowCap) {
    // Both sides under the cap — same as plain micro.
    FairValueEstimator fv{{.mode = Mode::kMicroSizeCapped, .size_cap_qty = 100.0}};
    auto book = make_book(100.0, 20.0, 102.0, 5.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.6);
}

// ---------------------------------------------------------------------------
// L2-weighted micro
// ---------------------------------------------------------------------------

TEST(FairValueEstimatorTest, L2WeightedSingleLevelEqualsMicro) {
    FairValueEstimator fv{{.mode = Mode::kL2WeightedMicro, .ladder_depth = 5, .ladder_decay = 0.5}};
    auto book = make_book(100.0, 10.0, 102.0, 10.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
}

TEST(FairValueEstimatorTest, L2WeightedDecayWeightedHandComputed) {
    // Bids (best→worst): (100, 10), (99, 20), (98, 30)
    // Asks (best→worst): (101, 5), (102, 15), (103, 25)
    // depth=3, decay=0.5
    // bid_w = 10*1 + 20*0.5 + 30*0.25 = 27.5
    // ask_w = 5*1 + 15*0.5 + 25*0.25 = 18.75
    // result = (100*18.75 + 101*27.5) / (27.5+18.75)
    //        = (1875 + 2777.5) / 46.25
    //        = 100.594594...
    FairValueEstimator fv{{.mode = Mode::kL2WeightedMicro, .ladder_depth = 3, .ladder_decay = 0.5}};
    OrderBookState book;
    book.apply({{100.0, 10.0}, {99.0, 20.0}, {98.0, 30.0}}, {{101.0, 5.0}, {102.0, 15.0}, {103.0, 25.0}}, 1, 1000);
    EXPECT_NEAR(fv.estimate(book), 100.594594594, 1e-6);
    // Sanity: more bid-weighted qty → tilt toward ask (above mid 100.5).
    EXPECT_GT(fv.last_estimate(), 100.5);
}

// ---------------------------------------------------------------------------
// EWMA-smoothed micro
// ---------------------------------------------------------------------------

TEST(FairValueEstimatorTest, EwmaFirstCallInitializesToCurrent) {
    // No previous state → EWMA must bootstrap to the current value, NOT
    // to alpha*x (which would mean a 0-anchored 30%-of-x answer).
    FairValueEstimator fv{{.mode = Mode::kEwmaMicro, .ewma_alpha = 0.3}};
    auto book = make_book(100.0, 10.0, 102.0, 10.0);  // micro = 101.0
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
}

TEST(FairValueEstimatorTest, EwmaConstantInputStaysConstant) {
    FairValueEstimator fv{{.mode = Mode::kEwmaMicro, .ewma_alpha = 0.3}};
    auto book = make_book(100.0, 10.0, 102.0, 10.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
}

TEST(FairValueEstimatorTest, EwmaStepInputLagsByAlpha) {
    FairValueEstimator fv{{.mode = Mode::kEwmaMicro, .ewma_alpha = 0.3}};

    // Bootstrap at micro=101.
    auto first = make_book(100.0, 10.0, 102.0, 10.0);
    EXPECT_DOUBLE_EQ(fv.estimate(first), 101.0);

    // Step to micro=111 (mid book of 110/112 with balanced qty).
    auto second = make_book(110.0, 10.0, 112.0, 10.0);
    // Expected: 0.3 * 111 + 0.7 * 101 = 33.3 + 70.7 = 104.0
    EXPECT_DOUBLE_EQ(fv.estimate(second), 104.0);

    // Same input again: 0.3 * 111 + 0.7 * 104 = 33.3 + 72.8 = 106.1
    EXPECT_NEAR(fv.estimate(second), 106.1, 1e-9);
}

TEST(FairValueEstimatorTest, EwmaSurvivesUnreadyBookGap) {
    // EWMA state must not be poisoned by a transient unready book in
    // the middle of a stream — the next valid call should re-anchor
    // smoothly, not bootstrap from NaN.
    FairValueEstimator fv{{.mode = Mode::kEwmaMicro, .ewma_alpha = 0.5}};
    auto book = make_book(100.0, 10.0, 102.0, 10.0);
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);  // bootstrap

    OrderBookState empty;
    EXPECT_TRUE(std::isnan(fv.estimate(empty)));  // gap

    // Resume: micro=101 again, prior ewma state was 101 — should still smooth.
    EXPECT_DOUBLE_EQ(fv.estimate(book), 101.0);
}

// ---------------------------------------------------------------------------
// Mode accessor
// ---------------------------------------------------------------------------

TEST(FairValueEstimatorTest, ModeAccessorReturnsConfiguredMode) {
    FairValueEstimator fv{{.mode = Mode::kMicro}};
    EXPECT_EQ(fv.mode(), Mode::kMicro);
}

// ---------------------------------------------------------------------------
// Default ctor + TOB (4-double) overload — used on the BBO hot path
// where there is no L2 ladder.
// ---------------------------------------------------------------------------

TEST(FairValueEstimatorTest, DefaultCtorIsMidMode) {
    FairValueEstimator fv;
    EXPECT_EQ(fv.mode(), Mode::kMid);
    EXPECT_DOUBLE_EQ(fv.estimate(100.0, 102.0, 10.0, 10.0), 101.0);
}

TEST(FairValueEstimatorTest, TobOverloadMicroMatchesBookOverload) {
    FairValueEstimator a{{.mode = Mode::kMicro}};
    FairValueEstimator b{{.mode = Mode::kMicro}};
    auto book = make_book(100.0, 20.0, 102.0, 5.0);
    EXPECT_DOUBLE_EQ(a.estimate(100.0, 102.0, 20.0, 5.0), b.estimate(book));
}

TEST(FairValueEstimatorTest, TobOverloadRejectsCrossedQuote) {
    FairValueEstimator fv{{.mode = Mode::kMicro}};
    EXPECT_TRUE(std::isnan(fv.estimate(102.0, 100.0, 10.0, 10.0)));  // crossed
    EXPECT_TRUE(std::isnan(fv.estimate(100.0, 100.0, 10.0, 10.0)));  // locked
    EXPECT_TRUE(std::isnan(fv.estimate(0.0, 102.0, 10.0, 10.0)));    // missing bid
    EXPECT_TRUE(std::isnan(fv.estimate(100.0, 0.0, 10.0, 10.0)));    // missing ask
}

TEST(FairValueEstimatorTest, TobOverloadL2ModeDegradesToPlainMicro) {
    // L2 weighting needs the ladder; via TOB overload it must fall back
    // to plain micro rather than producing garbage.
    FairValueEstimator l2{{.mode = Mode::kL2WeightedMicro, .ladder_depth = 5, .ladder_decay = 0.5}};
    FairValueEstimator m{{.mode = Mode::kMicro}};
    EXPECT_DOUBLE_EQ(l2.estimate(100.0, 102.0, 20.0, 5.0), m.estimate(100.0, 102.0, 20.0, 5.0));
}

}  // namespace
