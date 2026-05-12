#include "order_gateway/risk/pnl_tracker.h"

#include <gtest/gtest.h>

namespace {

using bpt::messages::ExchangeId;
using bpt::messages::OrderSide;
using bpt::order_gateway::risk::PnlTracker;

constexpr uint64_t kInst = 42;
constexpr int64_t kScale = 100'000'000;  // 1e8 (matches PnlTracker::kScale)

constexpr uint64_t ns_at(uint64_t day, uint64_t hour = 0) {
    return day * 86400ULL * 1'000'000'000ULL + hour * 3600ULL * 1'000'000'000ULL;
}

TEST(PnlTrackerTest, NoFillsZeroPnl) {
    PnlTracker t;
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 0.0);
    EXPECT_DOUBLE_EQ(t.session_realized_pnl_usd(), 0.0);
}

TEST(PnlTrackerTest, BuyThenSellProfit) {
    PnlTracker t;
    // Buy 1 @ $100 → long 1 @ avg 100.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 100 * kScale, 1 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 0.0);  // unrealized only
    // Sell 1 @ $110 → realized +$10.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 110 * kScale, 1 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 10.0);
    EXPECT_DOUBLE_EQ(t.session_realized_pnl_usd(), 10.0);
}

TEST(PnlTrackerTest, BuyThenSellLoss) {
    PnlTracker t;
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 100 * kScale, 1 * kScale, ns_at(1));
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 90 * kScale, 1 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), -10.0);
}

TEST(PnlTrackerTest, ShortThenCoverProfit) {
    PnlTracker t;
    // Short 1 @ $100.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 100 * kScale, 1 * kScale, ns_at(1));
    // Cover 1 @ $90 → realized +$10.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 90 * kScale, 1 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 10.0);
}

TEST(PnlTrackerTest, PartialClose) {
    PnlTracker t;
    // Buy 10 @ $100.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 100 * kScale, 10 * kScale, ns_at(1));
    // Sell 4 @ $110 → realized +$40 (on 4 units), still long 6.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 110 * kScale, 4 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 40.0);
    // Sell remaining 6 @ $90 → additional realized 6*(90-100) = -60 → daily -20.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 90 * kScale, 6 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), -20.0);
}

TEST(PnlTrackerTest, FlipLongToShortRealizesOnClosePortion) {
    PnlTracker t;
    // Buy 2 @ $100.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 100 * kScale, 2 * kScale, ns_at(1));
    // Sell 5 @ $120 → closes 2 long (realized +$40), flips to short 3 @ avg $120.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 120 * kScale, 5 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 40.0);
    // Cover 3 @ $110 → realizes short PnL 3*(120-110) = +30 → daily +70.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 110 * kScale, 3 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 70.0);
}

TEST(PnlTrackerTest, DailyRollsAtUtcMidnight) {
    PnlTracker t;
    // Day 1: realize -$10.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 100 * kScale, 1 * kScale, ns_at(1));
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 90 * kScale, 1 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), -10.0);
    // Day 2: query for daily resets; session still carries -$10.
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(2)), 0.0);
    EXPECT_DOUBLE_EQ(t.session_realized_pnl_usd(), -10.0);
    // Realize +$5 on day 2.
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 100 * kScale, 1 * kScale, ns_at(2));
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 105 * kScale, 1 * kScale, ns_at(2));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(2)), 5.0);
    EXPECT_DOUBLE_EQ(t.session_realized_pnl_usd(), -5.0);
}

TEST(PnlTrackerTest, DifferentInstrumentsTrackIndependently) {
    PnlTracker t;
    constexpr uint64_t instA = 1, instB = 2;
    t.on_fill(ExchangeId::OKX, instA, OrderSide::BUY, 100 * kScale, 1 * kScale, ns_at(1));
    t.on_fill(ExchangeId::OKX, instB, OrderSide::BUY, 50 * kScale, 1 * kScale, ns_at(1));
    // Selling instA doesn't touch instB's position.
    t.on_fill(ExchangeId::OKX, instA, OrderSide::SELL, 110 * kScale, 1 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 10.0);
    // Now sell instB for a loss.
    t.on_fill(ExchangeId::OKX, instB, OrderSide::SELL, 40 * kScale, 1 * kScale, ns_at(1));
    EXPECT_DOUBLE_EQ(t.daily_realized_pnl_usd(ns_at(1)), 0.0);
}

// ──────────────────────────────────────────────────────────────────────────
// net_qty_e8 accessor — used by the max_position_usd pretrade gate.
// ──────────────────────────────────────────────────────────────────────────

TEST(PnlTrackerTest, NetQtyZeroWhenUntouched) {
    PnlTracker t;
    EXPECT_EQ(t.net_qty_e8(ExchangeId::OKX, 999), 0);
}

TEST(PnlTrackerTest, NetQtyTracksLongPosition) {
    PnlTracker t;
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 100 * kScale, 3 * kScale, ns_at(1));
    EXPECT_EQ(t.net_qty_e8(ExchangeId::OKX, kInst), 3 * kScale);
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 110 * kScale, 2 * kScale, ns_at(1));
    EXPECT_EQ(t.net_qty_e8(ExchangeId::OKX, kInst), 5 * kScale);
}

TEST(PnlTrackerTest, NetQtyTracksShortPosition) {
    PnlTracker t;
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 100 * kScale, 2 * kScale, ns_at(1));
    EXPECT_EQ(t.net_qty_e8(ExchangeId::OKX, kInst), -2 * kScale);
}

TEST(PnlTrackerTest, NetQtyReturnsFlatAfterClose) {
    PnlTracker t;
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::BUY, 100 * kScale, 2 * kScale, ns_at(1));
    t.on_fill(ExchangeId::OKX, kInst, OrderSide::SELL, 110 * kScale, 2 * kScale, ns_at(1));
    EXPECT_EQ(t.net_qty_e8(ExchangeId::OKX, kInst), 0);
}

TEST(PnlTrackerTest, NetQtyIsolatedBetweenInstruments) {
    PnlTracker t;
    t.on_fill(ExchangeId::OKX, 1, OrderSide::BUY, 100 * kScale, 3 * kScale, ns_at(1));
    t.on_fill(ExchangeId::OKX, 2, OrderSide::SELL, 50 * kScale, 1 * kScale, ns_at(1));
    EXPECT_EQ(t.net_qty_e8(ExchangeId::OKX, 1), 3 * kScale);
    EXPECT_EQ(t.net_qty_e8(ExchangeId::OKX, 2), -1 * kScale);
    // And not spilled across exchanges.
    EXPECT_EQ(t.net_qty_e8(ExchangeId::HYPERLIQUID, 1), 0);
}

}  // namespace
