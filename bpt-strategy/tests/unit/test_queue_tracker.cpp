#include "strategy/strategy/queue_tracker.h"

#include <gtest/gtest.h>

namespace {

using bpt::messages::OrderSide;
using bpt::strategy::strategy::OrderBookState;
using bpt::strategy::strategy::QueueTracker;

// Helper: build a book with a simple bid/ask ladder.
OrderBookState make_book() {
    OrderBookState book;
    book.apply({{100.0, 5.0}, {99.9, 10.0}, {99.8, 15.0}}, {{100.1, 5.0}, {100.2, 10.0}, {100.3, 15.0}}, 1, 1000);
    return book;
}

TEST(QueueTrackerTest, TrackEmptyInitially) {
    QueueTracker t;
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(t.lookup(1), nullptr);
}

TEST(QueueTrackerTest, TrackBidAtBestUsesLevelSize) {
    QueueTracker t;
    OrderBookState book = make_book();
    // Place a bid at 100.0 for 1 BTC — book has 5 BTC sitting there already,
    // conservative model says all 5 BTC are ahead.
    t.track(42, OrderSide::BUY, 100.0, 1.0, 1000, book);
    const auto* e = t.lookup(42);
    ASSERT_NE(e, nullptr);
    EXPECT_DOUBLE_EQ(e->price, 100.0);
    EXPECT_DOUBLE_EQ(e->our_qty, 1.0);
    EXPECT_DOUBLE_EQ(e->queue_ahead, 5.0);
}

TEST(QueueTrackerTest, TrackBidBehindBestIncludesAllBetter) {
    QueueTracker t;
    OrderBookState book = make_book();
    // Place a bid at 99.9. Ahead = 5 (@100.0) + 10 (at 99.9 already) = 15
    t.track(1, OrderSide::BUY, 99.9, 1.0, 1000, book);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 15.0);
}

TEST(QueueTrackerTest, TrackAskAtBestUsesLevelSize) {
    QueueTracker t;
    OrderBookState book = make_book();
    // Place an ask at 100.1 for 1 BTC — 5 BTC ahead at that level.
    t.track(7, OrderSide::SELL, 100.1, 1.0, 1000, book);
    EXPECT_DOUBLE_EQ(t.lookup(7)->queue_ahead, 5.0);
}

TEST(QueueTrackerTest, TrackAskBehindBestIncludesAllBetter) {
    QueueTracker t;
    OrderBookState book = make_book();
    // Place an ask at 100.2. Ahead = 5 (@100.1) + 10 (@100.2 already) = 15
    t.track(7, OrderSide::SELL, 100.2, 1.0, 1000, book);
    EXPECT_DOUBLE_EQ(t.lookup(7)->queue_ahead, 15.0);
}

TEST(QueueTrackerTest, UnknownSideNotTracked) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::NULL_VALUE, 100.0, 1.0, 1000, book);
    EXPECT_EQ(t.size(), 0u);
}

// ---------------------------------------------------------------------------
// Fill / cancel lifecycle
// ---------------------------------------------------------------------------

TEST(QueueTrackerTest, PartialFillDecrementsOurQty) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::BUY, 100.0, 2.0, 1000, book);
    t.on_fill(1, 0.5);
    EXPECT_DOUBLE_EQ(t.lookup(1)->our_qty, 1.5);
}

TEST(QueueTrackerTest, FullFillDropsEntry) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::BUY, 100.0, 2.0, 1000, book);
    t.on_fill(1, 2.0);
    EXPECT_EQ(t.lookup(1), nullptr);
    EXPECT_EQ(t.size(), 0u);
}

TEST(QueueTrackerTest, CancelRemoves) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::BUY, 100.0, 2.0, 1000, book);
    t.on_cancel(1);
    EXPECT_EQ(t.size(), 0u);
}

// ---------------------------------------------------------------------------
// Trade-driven queue decrement
// ---------------------------------------------------------------------------

TEST(QueueTrackerTest, TradeAtBidHitsPassiveBidQueue) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::BUY, 100.0, 1.0, 1000, book);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 5.0);

    // Aggressive SELL at 100.0 for 2 BTC consumes 2 off the bid queue.
    t.on_trade(OrderSide::SELL, 100.0, 2.0, 2000);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 3.0);
}

TEST(QueueTrackerTest, TradeAtAskHitsPassiveAskQueue) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::SELL, 100.1, 1.0, 1000, book);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 5.0);

    t.on_trade(OrderSide::BUY, 100.1, 1.5, 2000);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 3.5);
}

TEST(QueueTrackerTest, TradeAtDifferentPriceNoEffect) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::BUY, 100.0, 1.0, 1000, book);

    // Trade at 99.9 (different level) — does not decrement our queue_ahead.
    t.on_trade(OrderSide::SELL, 99.9, 5.0, 2000);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 5.0);
}

TEST(QueueTrackerTest, TradeAggressorOnSameSideNoEffect) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::BUY, 100.0, 1.0, 1000, book);

    // Aggressive BUY at 100.0 — would consume ask-side passives, not our bid.
    t.on_trade(OrderSide::BUY, 100.0, 5.0, 2000);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 5.0);
}

TEST(QueueTrackerTest, QueueAheadClampedAtZero) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::BUY, 100.0, 1.0, 1000, book);

    // Trade bigger than the queue — clamped at 0.
    t.on_trade(OrderSide::SELL, 100.0, 99.0, 2000);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 0.0);
}

// ---------------------------------------------------------------------------
// Fill probability
// ---------------------------------------------------------------------------

TEST(QueueTrackerTest, FillProbabilityZeroIfUnknown) {
    QueueTracker t;
    EXPECT_DOUBLE_EQ(t.fill_probability(999), 0.0);
}

TEST(QueueTrackerTest, FillProbabilityOneIfAtFront) {
    QueueTracker t;
    OrderBookState book;
    // Empty ladder at our level → queue_ahead = 0.
    book.apply({}, {{100.1, 5.0}}, 1, 1000);
    t.track(1, OrderSide::BUY, 100.0, 1.0, 1000, book);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 0.0);
    EXPECT_DOUBLE_EQ(t.fill_probability(1), 1.0);
}

TEST(QueueTrackerTest, FillProbabilityRatio) {
    QueueTracker t;
    OrderBookState book = make_book();  // 5 BTC at our level 100.0
    t.track(1, OrderSide::BUY, 100.0, 1.0, 1000, book);
    // our=1, ahead=5 → 1 / (1 + 5) = 0.1667
    EXPECT_NEAR(t.fill_probability(1), 1.0 / 6.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Multiple orders
// ---------------------------------------------------------------------------

TEST(QueueTrackerTest, MultipleOrdersIndependent) {
    QueueTracker t;
    OrderBookState book = make_book();
    t.track(1, OrderSide::BUY, 100.0, 1.0, 1000, book);
    t.track(2, OrderSide::SELL, 100.1, 1.0, 1000, book);

    // Trade on bid side — only order 1 affected.
    t.on_trade(OrderSide::SELL, 100.0, 2.0, 2000);
    EXPECT_DOUBLE_EQ(t.lookup(1)->queue_ahead, 3.0);
    EXPECT_DOUBLE_EQ(t.lookup(2)->queue_ahead, 5.0);
}

}  // namespace
