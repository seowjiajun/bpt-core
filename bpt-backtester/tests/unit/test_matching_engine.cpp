// Unit tests for bpt::backtester::matching::MatchingEngine
#include "backtester/data/market_event.h"
#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"
#include "backtester/matching/matching_engine.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace bpt::backtester::matching;
using namespace bpt::backtester::data;

// ── Helpers ───────────────────────────────────────────────────────────────────

static MarketEvent make_book(const std::string& exchange,
                             const std::string& symbol,
                             double bid,
                             double ask,
                             double size = 10.0,
                             uint64_t ts = 1000) {
    OrderBookRecord ob;
    ob.timestamp_ns = ts;
    ob.exchange = exchange;
    ob.symbol = symbol;
    for (int i = 0; i < kOrderBookDepth; ++i) {
        ob.bid_px[i] = bid - i * 0.01;
        ob.bid_sz[i] = size;
        ob.ask_px[i] = ask + i * 0.01;
        ob.ask_sz[i] = size;
    }
    return MarketEvent::from_orderbook(ob);
}

static OpenOrder make_order(OrderType type,
                            OrderSide side,
                            double qty,
                            double price = 0.0,
                            const std::string& oid = "ord1",
                            const std::string& coid = "client1") {
    OpenOrder o;
    o.order_id = oid;
    o.client_order_id = coid;
    o.exchange = "BINANCE";
    o.symbol = "BTCUSDT";
    o.type = type;
    o.side = side;
    o.quantity = qty;
    o.price = price;
    return o;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, MarketBuyFillsAtBestAsk) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));

    auto order = make_order(OrderType::MARKET, OrderSide::BUY, 3.0);
    auto result = eng.submit_order(order);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 3.0);
    EXPECT_DOUBLE_EQ(fills[0].cumulative_fill_qty, 3.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
    EXPECT_DOUBLE_EQ(result.filled_qty, 3.0);
}

TEST(MatchingEngineTest, MarketSellFillsAtBestBid) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));

    auto result = eng.submit_order(make_order(OrderType::MARKET, OrderSide::SELL, 2.0));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 100.0);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 2.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
}

TEST(MatchingEngineTest, MarketOrderWalksMultipleLevels) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Each level has size 2.0; best ask=101.0, next=101.01, ...
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 2.0));

    eng.submit_order(make_order(OrderType::MARKET, OrderSide::BUY, 5.0));

    // 2 fills: 2@101.0 + 2@101.01 + 1@101.02 = 3 levels
    ASSERT_EQ(fills.size(), 3u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
    EXPECT_DOUBLE_EQ(fills[1].last_fill_price, 101.01);
    EXPECT_DOUBLE_EQ(fills[2].last_fill_price, 101.02);
    EXPECT_TRUE(fills.back().is_fully_filled);
}

TEST(MatchingEngineTest, LimitBuyQueuesAndFillsWhenAskCrosses) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Initial book: ask=102, bid=100 — doesn't cross limit of 101
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 102.0));

    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 101.0));
    EXPECT_EQ(fills.size(), 0u);  // not filled yet

    // Price drops: ask=101 (crosses limit)
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.5, 101.0, 10.0, 2000));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
}

TEST(MatchingEngineTest, LimitSellQueuesAndFillsWhenBidCrosses) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Initial book: bid=98, limit sell at 99 — doesn't cross
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 98.0, 100.0));

    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 1.0, 99.0));
    EXPECT_EQ(fills.size(), 0u);

    // Bid rises to 99 (crosses limit)
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 99.0, 101.0, 10.0, 2000));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 99.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
}

TEST(MatchingEngineTest, LimitOrderDoesNotFillAboveLimit) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 105.0));

    // Limit buy at 101, but ask is 105 — should not fill
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 101.0));

    // Tick with ask still above limit
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 101.0, 103.0, 10.0, 2000));
    EXPECT_EQ(fills.size(), 0u);
}

TEST(MatchingEngineTest, CancelRemovesOrder) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 105.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 101.0, "ord-cancel"));

    EXPECT_TRUE(eng.cancel_order("BINANCE", "BTCUSDT", "ord-cancel"));
    EXPECT_FALSE(eng.cancel_order("BINANCE", "BTCUSDT", "ord-cancel"));  // already gone

    // Price crosses — should not fire fill
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.5, 101.0, 10.0, 2000));
    EXPECT_EQ(fills.size(), 0u);
}

TEST(MatchingEngineTest, MarketOrderWithNoBookDoesNotCrash) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // No book update — market order arrives
    eng.submit_order(make_order(OrderType::MARKET, OrderSide::BUY, 1.0));
    EXPECT_EQ(fills.size(), 0u);  // no book → no fill, just a warning logged
}

TEST(MatchingEngineTest, FillReportFieldsAreCorrect) {
    MatchingEngine eng;
    FillReport captured;
    eng.set_fill_callback([&](FillReport r) { captured = r; });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.5, 10.0, 5000));

    OpenOrder o = make_order(OrderType::MARKET, OrderSide::BUY, 2.0, 0.0, "myOrd", "myCid");
    eng.submit_order(o);

    EXPECT_EQ(captured.order_id, "myOrd");
    EXPECT_EQ(captured.client_order_id, "myCid");
    EXPECT_EQ(captured.symbol, "BTCUSDT");
    EXPECT_EQ(captured.exchange, "BINANCE");
    EXPECT_EQ(captured.side, OrderSide::BUY);
    EXPECT_DOUBLE_EQ(captured.original_qty, 2.0);
    EXPECT_DOUBLE_EQ(captured.last_fill_price, 101.5);
    EXPECT_EQ(captured.simulation_ts, 5000u);
}
