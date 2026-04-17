// Unit tests for bpt::backtester::results::ResultsCollector
#include "backtester/data/market_event.h"
#include "backtester/data/orderbook_record.h"
#include "backtester/matching/open_order.h"
#include "backtester/results/results_collector.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace fs = std::filesystem;
using namespace bpt::backtester::results;
using namespace bpt::backtester::matching;
using namespace bpt::backtester::data;

// ── Helpers ───────────────────────────────────────────────────────────────────

static FillReport make_fill(OrderSide side,
                            double qty,
                            double price,
                            OrderType type = OrderType::LIMIT,
                            uint64_t ts = 1000) {
    FillReport r;
    r.order_id = "o1";
    r.client_order_id = "c1";
    r.exchange = "BINANCE";
    r.symbol = "BTCUSDT";
    r.order_type = type;
    r.side = side;
    r.original_qty = qty;
    r.order_price = price;
    r.last_fill_qty = qty;
    r.last_fill_price = price;
    r.cumulative_fill_qty = qty;
    r.is_fully_filled = true;
    r.simulation_ts = ts;
    return r;
}

static MarketEvent make_book_event(double bid, double ask, uint64_t ts = 2000) {
    OrderBookRecord ob;
    ob.timestamp_ns = ts;
    ob.exchange = "BINANCE";
    ob.symbol = "BTCUSDT";
    ob.bid_px[0] = bid;
    ob.bid_sz[0] = 10.0;
    ob.ask_px[0] = ask;
    ob.ask_sz[0] = 10.0;
    for (int i = 1; i < kOrderBookDepth; ++i) {
        ob.bid_px[i] = bid - i * 0.01;
        ob.bid_sz[i] = 10.0;
        ob.ask_px[i] = ask + i * 0.01;
        ob.ask_sz[i] = 10.0;
    }
    return MarketEvent::from_orderbook(ob);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(ResultsCollectorTest, FlatRoundTrip) {
    // BUY 1 @ 100, SELL 1 @ 110 → realized PnL = 10
    ResultsCollector rc(1000.0, "/tmp/jorm_rc_test");

    rc.on_fill(make_fill(OrderSide::BUY, 1.0, 100.0, OrderType::LIMIT, 1000));
    rc.on_fill(make_fill(OrderSide::SELL, 1.0, 110.0, OrderType::LIMIT, 2000));

    // No unrealized position → equity = 1000 + 10 = 1010
    rc.on_market_event(make_book_event(109.0, 111.0, 3000));
    // Mid = 110.0, position = 0, so equity stays 1010
    EXPECT_DOUBLE_EQ(rc.current_equity(), 1010.0);
}

TEST(ResultsCollectorTest, UnrealizedPnLFromBook) {
    ResultsCollector rc(1000.0, "/tmp/jorm_rc_test");

    rc.on_fill(make_fill(OrderSide::BUY, 2.0, 100.0, OrderType::LIMIT, 1000));
    // Position: long 2 @ avg 100
    // Mid price update: bid=109, ask=111 → mid=110
    rc.on_market_event(make_book_event(109.0, 111.0, 2000));
    // Unrealized = 2 * (110 - 100) = 20
    EXPECT_DOUBLE_EQ(rc.current_equity(), 1020.0);
}

TEST(ResultsCollectorTest, ShortPositionPnL) {
    ResultsCollector rc(1000.0, "/tmp/jorm_rc_test");

    rc.on_fill(make_fill(OrderSide::SELL, 1.0, 110.0, OrderType::LIMIT, 1000));
    // Position: short 1 @ avg 110
    rc.on_market_event(make_book_event(105.0, 107.0, 2000));  // mid = 106
    // Unrealized = -1 * (106 - 110) = +4
    EXPECT_DOUBLE_EQ(rc.current_equity(), 1004.0);
}

TEST(ResultsCollectorTest, FlipLongToShort) {
    ResultsCollector rc(1000.0, "/tmp/jorm_rc_test");

    rc.on_fill(make_fill(OrderSide::BUY, 2.0, 100.0, OrderType::LIMIT, 1000));
    // Sell 3: closes 2 long (realized = 2*(110-100)=20) + opens 1 short @ 110
    FillReport sell3 = make_fill(OrderSide::SELL, 3.0, 110.0, OrderType::LIMIT, 2000);
    sell3.last_fill_qty = 3.0;
    sell3.cumulative_fill_qty = 3.0;
    sell3.original_qty = 3.0;
    rc.on_fill(sell3);

    // No book yet → unrealized from short position = 0 (no mid price)
    // equity = 1000 + 20 = 1020
    EXPECT_DOUBLE_EQ(rc.current_equity(), 1020.0);

    // Now book update: mid = 100 → short 1 @ 110 is up 10
    rc.on_market_event(make_book_event(99.0, 101.0, 3000));
    // Unrealized = -1 * (100 - 110) = 10
    EXPECT_DOUBLE_EQ(rc.current_equity(), 1030.0);
}

TEST(ResultsCollectorTest, WriteCreatesFiles) {
    fs::path dir = fs::temp_directory_path() / "jorm_rc_write_test";
    fs::remove_all(dir);

    ResultsCollector rc(5000.0, dir.string());
    rc.on_fill(make_fill(OrderSide::BUY, 1.0, 200.0, OrderType::MARKET, 1000));
    rc.on_fill(make_fill(OrderSide::SELL, 1.0, 220.0, OrderType::LIMIT, 2000));
    rc.on_market_event(make_book_event(219.0, 221.0, 3000));

    ASSERT_NO_THROW(rc.write());

    EXPECT_TRUE(fs::exists(dir / "trades.csv"));
    EXPECT_TRUE(fs::exists(dir / "pnl_curve.csv"));
    EXPECT_TRUE(fs::exists(dir / "summary.json"));

    // trades.csv should have header + 2 data rows
    std::ifstream tf(dir / "trades.csv");
    int lines = 0;
    std::string line;
    while (std::getline(tf, line))
        ++lines;
    EXPECT_EQ(lines, 3);  // 1 header + 2 fills

    fs::remove_all(dir);
}

TEST(ResultsCollectorTest, MaxDrawdown) {
    ResultsCollector rc(1000.0, "/tmp/jorm_rc_dd_test");

    // equity goes: 1000 → 1100 → 900 → 1050
    // max DD = (1100 - 900) / 1100 ≈ 18.18%
    FillReport b1 = make_fill(OrderSide::BUY, 1.0, 100.0, OrderType::LIMIT, 1000);
    rc.on_fill(b1);
    rc.on_market_event(make_book_event(109.0, 111.0, 1100));  // mid=110, unrealized=+10
    // equity ≈ 1010 (unrealized 10)

    FillReport s1 = make_fill(OrderSide::SELL, 1.0, 110.0, OrderType::LIMIT, 2000);
    rc.on_fill(s1);  // realized +10, equity=1010

    FillReport b2 = make_fill(OrderSide::BUY, 1.0, 110.0, OrderType::LIMIT, 3000);
    rc.on_fill(b2);                                         // long 1@110
    rc.on_market_event(make_book_event(89.0, 91.0, 3100));  // mid=90, unrealized=-20
    // equity ≈ 990

    FillReport s2 = make_fill(OrderSide::SELL, 1.0, 90.0, OrderType::LIMIT, 4000);
    rc.on_fill(s2);  // realized -20, equity=990

    // max drawdown > 0 (there was a drawdown from 1010 to 990)
    EXPECT_GT(rc.compute_max_drawdown(), 0.0);
    EXPECT_LE(rc.compute_max_drawdown(), 1.0);
}
