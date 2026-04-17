// Component tests for BinanceParser.

#include "fake_md_publisher.h"
#include "md_gateway/adapter/binance/binance_parser.h"
#include "md_gateway/adapter/common/subscription_map.h"

#include <gtest/gtest.h>

namespace bpt::md_gateway::adapter {
namespace {

struct BinanceParserFixture {
    SubscriptionMap subs;
    BinanceParser parser{subs};
    test::FakeMdPublisher pub;
    messaging::FundingRateCallback fr_cb;

    void inject(const char* msg, uint64_t recv_ns = 0) { parser.parse(msg, recv_ns, pub, fr_cb); }
};

// ── BBO (bookTicker) ──────────────────────────────────────────────────────────

TEST(BinanceParserTest, BookTickerPublishesBbo) {
    BinanceParserFixture f;
    f.subs.subscribe(1001, "btcusdt");

    f.inject(R"({"stream":"btcusdt@bookTicker","data":{"b":"29990.50","B":"1.25","a":"29991.00","A":"0.75"}})",
             111111ULL);

    ASSERT_TRUE(f.pub.last_bbo.has_value());
    EXPECT_EQ(f.pub.last_bbo->instrument_id, 1001ULL);
    EXPECT_EQ(f.pub.last_bbo->timestamp_ns, 111111ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_price, 29990.50);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_qty, 1.25);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_price, 29991.00);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_qty, 0.75);
    EXPECT_FALSE(f.pub.last_trade.has_value());
}

// ── Trades (aggTrade) ─────────────────────────────────────────────────────────

TEST(BinanceParserTest, AggTradeBuyAggressor) {
    BinanceParserFixture f;
    f.subs.subscribe(1001, "btcusdt");

    // m=false → maker is seller → aggressor is buyer → BUY
    f.inject(R"({"stream":"btcusdt@aggTrade","data":{"p":"30000.00","q":"0.50","m":false}})", 222222ULL);

    ASSERT_TRUE(f.pub.last_trade.has_value());
    EXPECT_EQ(f.pub.last_trade->instrument_id, 1001ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_trade->price, 30000.00);
    EXPECT_DOUBLE_EQ(f.pub.last_trade->qty, 0.50);
    EXPECT_EQ(f.pub.last_trade->side, bpt::messages::TradeSide::BUY);
    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

TEST(BinanceParserTest, AggTradeSellAggressor) {
    BinanceParserFixture f;
    f.subs.subscribe(1001, "btcusdt");

    // m=true → maker is buyer → aggressor is seller → SELL
    f.inject(R"({"stream":"btcusdt@aggTrade","data":{"p":"29999.00","q":"2.00","m":true}})");

    ASSERT_TRUE(f.pub.last_trade.has_value());
    EXPECT_EQ(f.pub.last_trade->side, bpt::messages::TradeSide::SELL);
}

// ── Unknown / unsubscribed ────────────────────────────────────────────────────

TEST(BinanceParserTest, UnknownSymbolDropped) {
    BinanceParserFixture f;
    f.subs.subscribe(1001, "btcusdt");

    f.inject(R"({"stream":"ethusdt@bookTicker","data":{"b":"1800.00","B":"5.00","a":"1801.00","A":"3.00"}})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

TEST(BinanceParserTest, MultipleSymbols) {
    BinanceParserFixture f;
    f.subs.subscribe(1001, "btcusdt");
    f.subs.subscribe(1002, "ethusdt");

    f.inject(R"({"stream":"ethusdt@bookTicker","data":{"b":"1800.00","B":"5.00","a":"1801.00","A":"3.00"}})",
             444444ULL);

    ASSERT_TRUE(f.pub.last_bbo.has_value());
    EXPECT_EQ(f.pub.last_bbo->instrument_id, 1002ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_price, 1800.00);
}

}  // namespace
}  // namespace bpt::md_gateway::adapter
