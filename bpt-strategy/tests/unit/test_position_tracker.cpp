#include "strategy/strategy/position_tracker.h"

#include <gtest/gtest.h>

namespace {

using bpt::strategy::strategy::PositionTracker;
using bpt::messages::ExchangeId;
using bpt::messages::OrderSide;

constexpr uint64_t INST_BTC = 100;
constexpr uint64_t INST_ETH = 101;
constexpr auto BINANCE = ExchangeId::BINANCE;
constexpr auto OKX = ExchangeId::OKX;

// Encode a natural price/qty as 1e8 fixed-point (matching SBE encoding).
static uint64_t qty_fp(double q) { return static_cast<uint64_t>(q * 1e8); }
static int64_t px_fp(double p) { return static_cast<int64_t>(p * 1e8); }

// ---------------------------------------------------------------------------
// Empty state
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, EmptyByDefault) {
    PositionTracker tracker;
    EXPECT_FALSE(tracker.get(INST_BTC, BINANCE).has_value());
    EXPECT_EQ(tracker.net_qty(INST_BTC, BINANCE), 0);
}

// ---------------------------------------------------------------------------
// Single buy fill
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, SingleBuyCreatesLong) {
    PositionTracker tracker;
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.5), px_fp(50000.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, static_cast<int64_t>(qty_fp(1.5)));
    EXPECT_DOUBLE_EQ(pos->avg_price, 50000.0);
    EXPECT_DOUBLE_EQ(pos->realized_pnl, 0.0);
}

// ---------------------------------------------------------------------------
// Single sell fill
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, SingleSellCreatesShort) {
    PositionTracker tracker;
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(2.0), px_fp(60000.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, -static_cast<int64_t>(qty_fp(2.0)));
    EXPECT_DOUBLE_EQ(pos->avg_price, 60000.0);
    EXPECT_DOUBLE_EQ(pos->realized_pnl, 0.0);
}

// ---------------------------------------------------------------------------
// Average price on add
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, AveragePriceOnMultipleBuys) {
    PositionTracker tracker;
    // Buy 1 @ 100, then buy 1 @ 200 → avg = 150
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(200.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, static_cast<int64_t>(qty_fp(2.0)));
    EXPECT_NEAR(pos->avg_price, 150.0, 1e-6);
    EXPECT_DOUBLE_EQ(pos->realized_pnl, 0.0);
}

TEST(PositionTrackerTest, AveragePriceOnMultipleSells) {
    PositionTracker tracker;
    // Sell 2 @ 300, then sell 1 @ 600 → avg = 400
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(2.0), px_fp(300.0));
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(1.0), px_fp(600.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, -static_cast<int64_t>(qty_fp(3.0)));
    EXPECT_NEAR(pos->avg_price, 400.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Close for profit
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, CloseLongForProfit) {
    PositionTracker tracker;
    // Buy 1 @ 100
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    // Sell 1 @ 150 → realized PnL = 1 * (150 - 100) = 50
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(1.0), px_fp(150.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, 0);
    EXPECT_NEAR(pos->realized_pnl, 50.0, 1e-6);
}

TEST(PositionTrackerTest, CloseShortForProfit) {
    PositionTracker tracker;
    // Sell 1 @ 200
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(1.0), px_fp(200.0));
    // Buy 1 @ 150 → realized PnL = 1 * (200 - 150) = 50
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(150.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, 0);
    EXPECT_NEAR(pos->realized_pnl, 50.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Close for loss
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, CloseLongForLoss) {
    PositionTracker tracker;
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(1.0), px_fp(80.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    EXPECT_NEAR(pos->realized_pnl, -20.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Partial close
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, PartialClose) {
    PositionTracker tracker;
    // Buy 2 @ 100
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(2.0), px_fp(100.0));
    // Sell 1 @ 120 → realized PnL = 1 * (120 - 100) = 20, remaining 1 @ avg 100
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(1.0), px_fp(120.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, static_cast<int64_t>(qty_fp(1.0)));
    EXPECT_NEAR(pos->realized_pnl, 20.0, 1e-6);
    // avg_price for remaining position stays at original entry
    EXPECT_NEAR(pos->avg_price, 100.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Flip (close + reverse in one fill)
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, FlipLongToShort) {
    PositionTracker tracker;
    // Buy 1 @ 100
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    // Sell 3 @ 120 → close 1 for (120-100)=20 PnL, open short 2 @ 120
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(3.0), px_fp(120.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, -static_cast<int64_t>(qty_fp(2.0)));
    EXPECT_NEAR(pos->realized_pnl, 20.0, 1e-6);
    EXPECT_NEAR(pos->avg_price, 120.0, 1e-6);
}

TEST(PositionTrackerTest, FlipShortToLong) {
    PositionTracker tracker;
    // Sell 1 @ 200
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(1.0), px_fp(200.0));
    // Buy 2 @ 180 → close 1 for (200-180)=20 PnL, open long 1 @ 180
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(2.0), px_fp(180.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, static_cast<int64_t>(qty_fp(1.0)));
    EXPECT_NEAR(pos->realized_pnl, 20.0, 1e-6);
    EXPECT_NEAR(pos->avg_price, 180.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Multi-instrument isolation
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, InstrumentsAreIndependent) {
    PositionTracker tracker;
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    tracker.on_fill(INST_ETH, BINANCE, OrderSide::SELL, qty_fp(5.0), px_fp(3000.0));

    auto btc = tracker.get(INST_BTC, BINANCE);
    auto eth = tracker.get(INST_ETH, BINANCE);
    ASSERT_TRUE(btc.has_value());
    ASSERT_TRUE(eth.has_value());
    EXPECT_GT(btc->net_qty, 0);
    EXPECT_LT(eth->net_qty, 0);
}

TEST(PositionTrackerTest, ExchangesAreIndependent) {
    PositionTracker tracker;
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    tracker.on_fill(INST_BTC, OKX, OrderSide::SELL, qty_fp(2.0), px_fp(200.0));

    auto binance = tracker.get(INST_BTC, BINANCE);
    auto okx = tracker.get(INST_BTC, OKX);
    ASSERT_TRUE(binance.has_value());
    ASSERT_TRUE(okx.has_value());
    EXPECT_GT(binance->net_qty, 0);
    EXPECT_LT(okx->net_qty, 0);
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, ClearSingleInstrument) {
    PositionTracker tracker;
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    tracker.on_fill(INST_ETH, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));

    tracker.clear(INST_BTC, BINANCE);
    EXPECT_FALSE(tracker.get(INST_BTC, BINANCE).has_value());
    EXPECT_TRUE(tracker.get(INST_ETH, BINANCE).has_value());
}

TEST(PositionTrackerTest, ClearAll) {
    PositionTracker tracker;
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    tracker.on_fill(INST_ETH, OKX, OrderSide::SELL, qty_fp(1.0), px_fp(100.0));

    tracker.clear_all();
    EXPECT_FALSE(tracker.get(INST_BTC, BINANCE).has_value());
    EXPECT_FALSE(tracker.get(INST_ETH, OKX).has_value());
}

// ---------------------------------------------------------------------------
// Cumulative PnL across multiple round-trips
// ---------------------------------------------------------------------------

TEST(PositionTrackerTest, CumulativePnlAcrossRoundTrips) {
    PositionTracker tracker;
    // Round trip 1: buy 1 @ 100, sell 1 @ 110 → +10
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(100.0));
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(1.0), px_fp(110.0));
    // Round trip 2: sell 1 @ 200, buy 1 @ 190 → +10
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::SELL, qty_fp(1.0), px_fp(200.0));
    tracker.on_fill(INST_BTC, BINANCE, OrderSide::BUY, qty_fp(1.0), px_fp(190.0));

    auto pos = tracker.get(INST_BTC, BINANCE);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->net_qty, 0);
    EXPECT_NEAR(pos->realized_pnl, 20.0, 1e-6);
}

}  // namespace
