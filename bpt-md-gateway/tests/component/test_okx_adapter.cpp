// Component tests for OkxParser.

#include "fake_md_publisher.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/adapter/okx/okx_parser.h"

#include <gtest/gtest.h>

namespace bpt::md_gateway::adapter {
namespace {

struct OkxParserFixture {
    SubscriptionMap subs;
    OkxParser parser{subs};
    test::FakeMdPublisher pub;
    messaging::FundingRateCallback fr_cb;

    void inject(const char* msg, uint64_t recv_ns = 0) { parser.parse(msg, recv_ns, pub, fr_cb); }
};

// ── BBO (books5) ──────────────────────────────────────────────────────────────

TEST(OkxParserTest, Books5PublishesBbo) {
    OkxParserFixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP", 5);

    f.inject(
        R"({"arg":{"channel":"books5","instId":"BTC-USDT-SWAP"},"data":[{"bids":[["29990","1.5","0","1"]],"asks":[["29991","0.8","0","1"]]}]})",
        123456789ULL);

    ASSERT_TRUE(f.pub.last_bbo.has_value());
    EXPECT_EQ(f.pub.last_bbo->instrument_id, 1001ULL);
    EXPECT_EQ(f.pub.last_bbo->timestamp_ns, 123456789ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_price, 29990.0);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_qty, 1.5);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_price, 29991.0);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_qty, 0.8);
    EXPECT_FALSE(f.pub.last_trade.has_value());
}

TEST(OkxParserTest, Books5EmptyBidsDropped) {
    OkxParserFixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP", 5);

    f.inject(
        R"({"arg":{"channel":"books5","instId":"BTC-USDT-SWAP"},"data":[{"bids":[],"asks":[["29991","0.8","0","1"]]}]})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

// ── Trades ────────────────────────────────────────────────────────────────────

TEST(OkxParserTest, TradesSellSide) {
    OkxParserFixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP");

    f.inject(
        R"({"arg":{"channel":"trades","instId":"BTC-USDT-SWAP"},"data":[{"px":"30000","sz":"0.3","side":"sell"}]})",
        999999ULL);

    ASSERT_TRUE(f.pub.last_trade.has_value());
    EXPECT_EQ(f.pub.last_trade->instrument_id, 1001ULL);
    EXPECT_EQ(f.pub.last_trade->timestamp_ns, 999999ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_trade->price, 30000.0);
    EXPECT_DOUBLE_EQ(f.pub.last_trade->qty, 0.3);
    EXPECT_EQ(f.pub.last_trade->side, bpt::messages::TradeSide::SELL);
    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

TEST(OkxParserTest, TradesBuySide) {
    OkxParserFixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP");

    f.inject(
        R"({"arg":{"channel":"trades","instId":"BTC-USDT-SWAP"},"data":[{"px":"30100","sz":"1.0","side":"buy"}]})");

    ASSERT_TRUE(f.pub.last_trade.has_value());
    EXPECT_EQ(f.pub.last_trade->side, bpt::messages::TradeSide::BUY);
}

// ── Event messages ────────────────────────────────────────────────────────────

TEST(OkxParserTest, SubscribeAckIgnored) {
    OkxParserFixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP");

    f.inject(R"({"event":"subscribe","arg":{"channel":"books5","instId":"BTC-USDT-SWAP"}})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
    EXPECT_FALSE(f.pub.last_trade.has_value());
}

// ── Unknown symbol ────────────────────────────────────────────────────────────

TEST(OkxParserTest, UnknownSymbolDropped) {
    OkxParserFixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP");

    f.inject(
        R"({"arg":{"channel":"books5","instId":"ETH-USDT-SWAP"},"data":[{"bids":[["1800","1.0","0","1"]],"asks":[["1801","0.5","0","1"]]}]})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

// ── Unsubscribed symbol ───────────────────────────────────────────────────────

TEST(OkxParserTest, UnsubscribedSymbolDropped) {
    OkxParserFixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP");
    f.subs.unsubscribe(1001);

    f.inject(
        R"({"arg":{"channel":"books5","instId":"BTC-USDT-SWAP"},"data":[{"bids":[["29990","1.5","0","1"]],"asks":[["29991","0.8","0","1"]]}]})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

}  // namespace
}  // namespace bpt::md_gateway::adapter
