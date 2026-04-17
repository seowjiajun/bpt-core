// Component tests for HyperliquidParser.

#include "fake_md_publisher.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_parser.h"

#include <gtest/gtest.h>

namespace bpt::md_gateway::adapter {
namespace {

struct HyperliquidParserFixture {
    SubscriptionMap subs;
    HyperliquidParser parser{subs};
    test::FakeMdPublisher pub;
    messaging::FundingRateCallback fr_cb;

    void inject(const char* msg, uint64_t recv_ns = 0) { parser.parse(msg, recv_ns, pub, fr_cb); }
};

// ── BBO (l2Book) ──────────────────────────────────────────────────────────────

TEST(HyperliquidParserTest, L2BookPublishesBbo) {
    HyperliquidParserFixture f;
    f.subs.subscribe(1001, "BTC");

    f.inject(
        R"({"channel":"l2Book","data":{"coin":"BTC","levels":[[{"px":"29990","sz":"1.5"},{"px":"29989","sz":"2.0"}],[{"px":"29991","sz":"0.8"}]]}})",
        666666ULL);

    ASSERT_TRUE(f.pub.last_bbo.has_value());
    EXPECT_EQ(f.pub.last_bbo->instrument_id, 1001ULL);
    EXPECT_EQ(f.pub.last_bbo->timestamp_ns, 666666ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_price, 29990.0);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->bid_qty, 1.5);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_price, 29991.0);
    EXPECT_DOUBLE_EQ(f.pub.last_bbo->ask_qty, 0.8);
    EXPECT_FALSE(f.pub.last_trade.has_value());
}

// ── Trades ────────────────────────────────────────────────────────────────────

TEST(HyperliquidParserTest, TradeBuySide) {
    HyperliquidParserFixture f;
    f.subs.subscribe(1001, "BTC");

    f.inject(R"({"channel":"trades","data":[{"coin":"BTC","px":"30000","sz":"0.5","side":"B"}]})", 777777ULL);

    ASSERT_TRUE(f.pub.last_trade.has_value());
    EXPECT_EQ(f.pub.last_trade->instrument_id, 1001ULL);
    EXPECT_EQ(f.pub.last_trade->timestamp_ns, 777777ULL);
    EXPECT_DOUBLE_EQ(f.pub.last_trade->price, 30000.0);
    EXPECT_DOUBLE_EQ(f.pub.last_trade->qty, 0.5);
    EXPECT_EQ(f.pub.last_trade->side, bpt::messages::TradeSide::BUY);
    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

TEST(HyperliquidParserTest, TradeSellSide) {
    HyperliquidParserFixture f;
    f.subs.subscribe(1001, "BTC");

    // side "A" = ask aggressor = sell
    f.inject(R"({"channel":"trades","data":[{"coin":"BTC","px":"29900","sz":"1.0","side":"A"}]})");

    ASSERT_TRUE(f.pub.last_trade.has_value());
    EXPECT_EQ(f.pub.last_trade->side, bpt::messages::TradeSide::SELL);
}

// ── Unknown / unsubscribed ────────────────────────────────────────────────────

TEST(HyperliquidParserTest, UnknownCoinDropped) {
    HyperliquidParserFixture f;
    f.subs.subscribe(1001, "BTC");

    f.inject(R"({"channel":"trades","data":[{"coin":"ETH","px":"1800","sz":"1.0","side":"B"}]})");

    EXPECT_FALSE(f.pub.last_trade.has_value());
}

TEST(HyperliquidParserTest, L2BookInsufficientLevelsDropped) {
    HyperliquidParserFixture f;
    f.subs.subscribe(1001, "BTC");

    // Only one side in levels — should not publish
    f.inject(R"({"channel":"l2Book","data":{"coin":"BTC","levels":[[{"px":"29990","sz":"1.5"}]]}})");

    EXPECT_FALSE(f.pub.last_bbo.has_value());
}

}  // namespace
}  // namespace bpt::md_gateway::adapter
