// Component tests for DeribitParser.

#include "fake_md_publisher.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/adapter/deribit/deribit_parser.h"

#include <gtest/gtest.h>

namespace bpt::md_gateway::adapter {
namespace {

struct DeribitParserFixture {
    SubscriptionMap subs;
    DeribitParser parser{subs};
    test::FakeMdPublisher pub;
    messaging::FundingRateCallback fr_cb;

    void inject(const char* msg, uint64_t recv_ns = 0) { parser.parse(msg, recv_ns, pub, fr_cb); }
};

// ── BBO (quote channel) ───────────────────────────────────────────────────────

TEST(DeribitParserTest, QuotePublishesBbo) {
    DeribitParserFixture f;
    f.subs.subscribe(3001, "BTC-PERPETUAL");

    f.inject(
        R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"quote.BTC-PERPETUAL","data":{"best_bid_price":29990.0,"best_bid_amount":1.5,"best_ask_price":29991.0,"best_ask_amount":0.8}}})",
        111111ULL);

    ASSERT_TRUE(f.pub.last_bbo.has_value());
    EXPECT_EQ(f.pub.last_bbo->instrument_id, 3001ULL);
    EXPECT_EQ(f.pub.last_bbo->timestamp_ns, 111111ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_price, 29990.0);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_qty, 1.5);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_price, 29991.0);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_qty, 0.8);
    EXPECT_FALSE(f.pub.last_trade.has_value());
}

// ── Trades ────────────────────────────────────────────────────────────────────

TEST(DeribitParserTest, TradesSellSide) {
    DeribitParserFixture f;
    f.subs.subscribe(3001, "BTC-PERPETUAL");

    f.inject(
        R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"trades.BTC-PERPETUAL.100ms","data":[{"price":30000.0,"amount":0.3,"direction":"sell"}]}})",
        222222ULL);

    ASSERT_TRUE(f.pub.last_trade.has_value());
    EXPECT_EQ(f.pub.last_trade->instrument_id, 3001ULL);
    EXPECT_EQ(f.pub.last_trade->timestamp_ns, 222222ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_trade->price, 30000.0);
    EXPECT_DOUBLE_EQ(f.pub.last_trade->qty, 0.3);
    EXPECT_EQ(f.pub.last_trade->side, bpt::messages::TradeSide::SELL);
    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

TEST(DeribitParserTest, TradesBuySide) {
    DeribitParserFixture f;
    f.subs.subscribe(3001, "BTC-PERPETUAL");

    f.inject(
        R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"trades.BTC-PERPETUAL.100ms","data":[{"price":30100.0,"amount":1.0,"direction":"buy"}]}})");

    ASSERT_TRUE(f.pub.last_trade.has_value());
    EXPECT_EQ(f.pub.last_trade->side, bpt::messages::TradeSide::BUY);
}

// ── Order Book (book channel) ─────────────────────────────────────────────────

TEST(DeribitParserTest, BookSnapshotPublishesOrderBookAndBbo) {
    DeribitParserFixture f;
    f.subs.subscribe(3001, "BTC-PERPETUAL", 5);

    f.inject(
        R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"book.BTC-PERPETUAL.100ms","data":{"type":"snapshot","change_id":1000,"bids":[[29990.0,1.5],[29989.0,2.0]],"asks":[[29991.0,0.8],[29992.0,1.2]]}}})",
        333333ULL);

    ASSERT_TRUE(f.pub.last_order_book.has_value());
    EXPECT_EQ(f.pub.last_order_book->instrument_id, 3001ULL);
    ASSERT_GE(f.pub.last_order_book->bids.size(), 1u);
    EXPECT_DOUBLE_EQ(f.pub.last_order_book->bids[0].first, 29990.0);
    EXPECT_DOUBLE_EQ(f.pub.last_order_book->bids[0].second, 1.5);

    ASSERT_TRUE(f.pub.last_bbo.has_value());
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_price, 29990.0);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_price, 29991.0);
}

// ── Heartbeat test_request ────────────────────────────────────────────────────

TEST(DeribitParserTest, HeartbeatTestRequestSetsFlag) {
    DeribitParserFixture f;

    EXPECT_FALSE(f.parser.take_test_request());

    f.inject(R"({"jsonrpc":"2.0","method":"heartbeat","params":{"type":"test_request"}})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
    EXPECT_FALSE(f.pub.last_trade.has_value());
    EXPECT_TRUE(f.parser.take_test_request());
    // Flag is consumed — second call returns false
    EXPECT_FALSE(f.parser.take_test_request());
}

// ── JSON-RPC response (subscribe ack) ────────────────────────────────────────

TEST(DeribitParserTest, SubscribeResponseIgnored) {
    DeribitParserFixture f;
    f.subs.subscribe(3001, "BTC-PERPETUAL");

    f.inject(R"({"jsonrpc":"2.0","id":1,"result":{"subscriptions":["quote.BTC-PERPETUAL"]}})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

// ── Unknown symbol dropped ────────────────────────────────────────────────────

TEST(DeribitParserTest, UnknownSymbolDropped) {
    DeribitParserFixture f;
    f.subs.subscribe(3001, "BTC-PERPETUAL");

    f.inject(
        R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"quote.ETH-PERPETUAL","data":{"best_bid_price":1800.0,"best_bid_amount":5.0,"best_ask_price":1801.0,"best_ask_amount":3.0}}})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

}  // namespace
}  // namespace bpt::md_gateway::adapter
