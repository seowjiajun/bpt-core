#include "bpt_common/book/order_book_state.h"

#include <gtest/gtest.h>

namespace {

using bpt::common::book::OrderBookState;
using Level = OrderBookState::Level;

// ---------------------------------------------------------------------------
// Warmup / readiness
// ---------------------------------------------------------------------------

TEST(OrderBookStateTest, EmptyStateNotReady) {
    OrderBookState book;
    EXPECT_FALSE(book.ready());
    EXPECT_EQ(book.n_bid_levels(), 0u);
    EXPECT_EQ(book.n_ask_levels(), 0u);
    EXPECT_EQ(book.last_seq_num(), 0u);
}

TEST(OrderBookStateTest, OnlyBidsStillNotReady) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {}, 1, 1000);
    EXPECT_FALSE(book.ready());
}

TEST(OrderBookStateTest, OnlyAsksStillNotReady) {
    OrderBookState book;
    book.apply({}, {{101.0, 1.0}}, 1, 1000);
    EXPECT_FALSE(book.ready());
}

TEST(OrderBookStateTest, BothSidesReady) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.0, 1.0}}, 1, 1000);
    EXPECT_TRUE(book.ready());
    EXPECT_DOUBLE_EQ(book.best_bid(), 100.0);
    EXPECT_DOUBLE_EQ(book.best_ask(), 101.0);
    EXPECT_DOUBLE_EQ(book.mid(), 100.5);
}

// ---------------------------------------------------------------------------
// Level ordering
// ---------------------------------------------------------------------------

TEST(OrderBookStateTest, BidsSortedDescending) {
    OrderBookState book;
    book.apply({{99.5, 1.0}, {100.0, 2.0}, {99.8, 3.0}}, {{101.0, 1.0}}, 1, 1000);
    auto tops = book.top_bids(3);
    ASSERT_EQ(tops.size(), 3u);
    EXPECT_DOUBLE_EQ(tops[0].price, 100.0);
    EXPECT_DOUBLE_EQ(tops[1].price, 99.8);
    EXPECT_DOUBLE_EQ(tops[2].price, 99.5);
    EXPECT_DOUBLE_EQ(book.best_bid(), 100.0);
    EXPECT_DOUBLE_EQ(book.best_bid_qty(), 2.0);
}

TEST(OrderBookStateTest, AsksSortedAscending) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.5, 1.0}, {101.0, 2.0}, {101.3, 3.0}}, 1, 1000);
    auto tops = book.top_asks(3);
    ASSERT_EQ(tops.size(), 3u);
    EXPECT_DOUBLE_EQ(tops[0].price, 101.0);
    EXPECT_DOUBLE_EQ(tops[1].price, 101.3);
    EXPECT_DOUBLE_EQ(tops[2].price, 101.5);
    EXPECT_DOUBLE_EQ(book.best_ask(), 101.0);
    EXPECT_DOUBLE_EQ(book.best_ask_qty(), 2.0);
}

// ---------------------------------------------------------------------------
// Delta merging
// ---------------------------------------------------------------------------

TEST(OrderBookStateTest, DeltaUpdatesExistingLevel) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.0, 1.0}}, 1, 1000);
    book.apply({{100.0, 5.0}}, {}, 2, 2000);
    EXPECT_DOUBLE_EQ(book.best_bid_qty(), 5.0);
    EXPECT_EQ(book.last_seq_num(), 2u);
    EXPECT_EQ(book.last_update_ns(), 2000u);
}

TEST(OrderBookStateTest, ZeroQtyRemovesLevel) {
    OrderBookState book;
    book.apply({{100.0, 1.0}, {99.5, 2.0}}, {{101.0, 1.0}}, 1, 1000);
    // Remove 100.0 via qty=0 delta.
    book.apply({{100.0, 0.0}}, {}, 2, 2000);
    EXPECT_EQ(book.n_bid_levels(), 1u);
    EXPECT_DOUBLE_EQ(book.best_bid(), 99.5);
}

TEST(OrderBookStateTest, DeltaAddsNewLevel) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.0, 1.0}}, 1, 1000);
    book.apply({{99.5, 3.0}}, {}, 2, 2000);
    EXPECT_EQ(book.n_bid_levels(), 2u);
    EXPECT_DOUBLE_EQ(book.size_at_bid(99.5), 3.0);
}

TEST(OrderBookStateTest, ZeroQtyOnNonexistentLevelIsNoop) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.0, 1.0}}, 1, 1000);
    book.apply({{95.0, 0.0}}, {}, 2, 2000);
    EXPECT_EQ(book.n_bid_levels(), 1u);
    EXPECT_EQ(book.last_seq_num(), 2u);
}

// ---------------------------------------------------------------------------
// size_at lookups
// ---------------------------------------------------------------------------

TEST(OrderBookStateTest, SizeAtMissingPriceReturnsZero) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.0, 1.0}}, 1, 1000);
    EXPECT_DOUBLE_EQ(book.size_at_bid(99.9), 0.0);
    EXPECT_DOUBLE_EQ(book.size_at_ask(102.0), 0.0);
}

TEST(OrderBookStateTest, SizeAtPresentPrice) {
    OrderBookState book;
    book.apply({{100.0, 1.5}}, {{101.0, 2.5}}, 1, 1000);
    EXPECT_DOUBLE_EQ(book.size_at_bid(100.0), 1.5);
    EXPECT_DOUBLE_EQ(book.size_at_ask(101.0), 2.5);
}

// ---------------------------------------------------------------------------
// Cumulative vol helpers
// ---------------------------------------------------------------------------

TEST(OrderBookStateTest, BidVolAboveExcludesEqualPrice) {
    OrderBookState book;
    book.apply({{100.3, 1.0}, {100.2, 2.0}, {100.1, 3.0}, {100.0, 4.0}}, {{101.0, 1.0}}, 1, 1000);
    // Strictly above 100.1: 100.3 + 100.2 = 3.0
    EXPECT_DOUBLE_EQ(book.bid_vol_above(100.1), 3.0);
    // Strictly above 100.4 (outside ladder): 0
    EXPECT_DOUBLE_EQ(book.bid_vol_above(100.4), 0.0);
    // Strictly above 99.0 (below bottom): all of it = 10.0
    EXPECT_DOUBLE_EQ(book.bid_vol_above(99.0), 10.0);
    // Strictly above 100.3 (top): 0.0 (nothing above the top bid)
    EXPECT_DOUBLE_EQ(book.bid_vol_above(100.3), 0.0);
}

TEST(OrderBookStateTest, AskVolBelowExcludesEqualPrice) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.0, 1.0}, {101.1, 2.0}, {101.2, 3.0}, {101.3, 4.0}}, 1, 1000);
    // Strictly below 101.2: 101.0 + 101.1 = 3.0
    EXPECT_DOUBLE_EQ(book.ask_vol_below(101.2), 3.0);
    // Strictly below 101.0 (top): 0
    EXPECT_DOUBLE_EQ(book.ask_vol_below(101.0), 0.0);
    // Strictly below 102.0: all = 10.0
    EXPECT_DOUBLE_EQ(book.ask_vol_below(102.0), 10.0);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(OrderBookStateTest, ResetClearsEverything) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.0, 1.0}}, 42, 1000);
    book.reset();
    EXPECT_FALSE(book.ready());
    EXPECT_EQ(book.last_seq_num(), 0u);
    EXPECT_EQ(book.last_update_ns(), 0u);
}

// ---------------------------------------------------------------------------
// Snapshot semantics — is_snapshot=true clears before folding
// ---------------------------------------------------------------------------

TEST(OrderBookStateTest, SnapshotReplacesStaleLevels) {
    OrderBookState book;
    // First frame as delta — levels accumulate.
    book.apply({{100.0, 1.0}, {99.9, 2.0}, {99.8, 3.0}}, {{100.1, 1.0}, {100.2, 2.0}, {100.3, 3.0}}, 1, 1000);
    EXPECT_EQ(book.n_bid_levels(), 3u);

    // Second frame as SNAPSHOT — only top-2 levels on each side. The
    // old levels (99.8, 100.3) should NOT persist. On OKX books5, this
    // is what happens when the book shifts and older levels fall out of
    // the top-5 window — a delta-only merge would keep them forever.
    book.apply({{100.0, 5.0}, {99.9, 5.0}},
               {{100.1, 5.0}, {100.2, 5.0}},
               2,
               2000,
               /*is_snapshot=*/true);
    EXPECT_EQ(book.n_bid_levels(), 2u);
    EXPECT_EQ(book.n_ask_levels(), 2u);
    EXPECT_DOUBLE_EQ(book.size_at_bid(99.8), 0.0);
    EXPECT_DOUBLE_EQ(book.size_at_ask(100.3), 0.0);
    EXPECT_DOUBLE_EQ(book.size_at_bid(100.0), 5.0);
}

TEST(OrderBookStateTest, SnapshotOnEmptyStateIsSameAsDelta) {
    OrderBookState book;
    book.apply({{100.0, 1.0}}, {{101.0, 1.0}}, 1, 1000, /*is_snapshot=*/true);
    EXPECT_TRUE(book.ready());
    EXPECT_DOUBLE_EQ(book.best_bid(), 100.0);
}

TEST(OrderBookStateTest, DeltaAfterSnapshotStillFolds) {
    OrderBookState book;
    book.apply({{100.0, 1.0}, {99.9, 2.0}}, {{100.1, 1.0}, {100.2, 2.0}}, 1, 1000, /*is_snapshot=*/true);
    // Follow up with a delta that removes one level.
    book.apply({{99.9, 0.0}}, {}, 2, 2000, /*is_snapshot=*/false);
    EXPECT_EQ(book.n_bid_levels(), 1u);
    EXPECT_DOUBLE_EQ(book.best_bid(), 100.0);
}

// ---------------------------------------------------------------------------
// Realistic scenario: mini-book with churn
// ---------------------------------------------------------------------------

TEST(OrderBookStateTest, MiniBookScenario) {
    OrderBookState book;
    // Initial snapshot-like delta: 3 bid, 3 ask levels.
    book.apply({{99.9, 1.0}, {99.8, 2.0}, {99.7, 3.0}}, {{100.1, 1.0}, {100.2, 2.0}, {100.3, 3.0}}, 1, 1000);
    EXPECT_DOUBLE_EQ(book.mid(), 100.0);

    // Bid 99.9 trades out, bid 99.85 appears behind it.
    book.apply({{99.9, 0.0}, {99.85, 4.0}}, {}, 2, 2000);
    EXPECT_DOUBLE_EQ(book.best_bid(), 99.85);
    EXPECT_DOUBLE_EQ(book.size_at_bid(99.85), 4.0);

    // Ask 100.1 size change.
    book.apply({}, {{100.1, 5.0}}, 3, 3000);
    EXPECT_DOUBLE_EQ(book.best_ask_qty(), 5.0);

    // Someone joins the bid at 99.85 with more size.
    book.apply({{99.85, 7.0}}, {}, 4, 4000);
    EXPECT_DOUBLE_EQ(book.size_at_bid(99.85), 7.0);

    // Queue ahead for a bid placed at 99.80 = sum of 99.85 = 7.0
    EXPECT_DOUBLE_EQ(book.bid_vol_above(99.80), 7.0);
}

}  // namespace
