// Smoke tests verifying the parser classes are reachable and produce correct
// output for the most basic cases.  Full coverage lives in the component tests.

#include "md_gateway/adapter/binance/binance_md_decoder.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_decoder.h"
#include "md_gateway/adapter/okx/okx_md_decoder.h"
#include "md_gateway/md/md_types.h"
#include "md_gateway/messaging/funding_rate_publisher.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>

namespace {

namespace adapter = bpt::md_gateway::adapter;
namespace messaging = bpt::md_gateway::messaging;
namespace md = bpt::md_gateway::md;

// Minimal publisher that captures the last published BBO and trade. No
// virtual interface — decoders are templated on the publisher type.
struct CapturePub {
    void publish(const md::MdBbo& bbo) { last_bbo = bbo; }
    void publish(const md::MdTrade& trade) { last_trade = trade; }
    void publish(const md::MdOrderBook&) {}
    uint64_t drop_count() const { return 0; }

    std::optional<md::MdBbo> last_bbo;
    std::optional<md::MdTrade> last_trade;
};

// ── Binance ───────────────────────────────────────────────────────────────────

TEST(AdapterSmokeTest, BinanceBookTicker) {
    adapter::SubscriptionMap subs;
    subs.subscribe(100, "btcusdt");
    adapter::BinanceMdDecoder<CapturePub> parser(subs);
    CapturePub pub;
    messaging::FundingRateCallback fr;

    parser.decode(R"({"stream":"btcusdt@bookTicker","data":{"b":"29990.50","B":"1.25","a":"29991.00","A":"0.75"}})",
                  0,
                  pub,
                  fr);

    ASSERT_TRUE(pub.last_bbo.has_value());
    EXPECT_EQ(pub.last_bbo->instrument_id, 100ULL);
    EXPECT_DOUBLE_EQ(pub.last_bbo->bid_price, 29990.50);
    EXPECT_DOUBLE_EQ(pub.last_bbo->ask_price, 29991.00);
}

// ── OKX ───────────────────────────────────────────────────────────────────────

TEST(AdapterSmokeTest, OkxBooks5) {
    adapter::SubscriptionMap subs;
    subs.subscribe(200, "BTC-USDT-SWAP", 5);
    adapter::OkxMdDecoder<CapturePub> parser(subs);
    CapturePub pub;
    messaging::FundingRateCallback fr;

    parser.decode(
        R"({"arg":{"channel":"books5","instId":"BTC-USDT-SWAP"},"data":[{"bids":[["29990","1.5","0","1"]],"asks":[["29991","0.8","0","1"]]}]})",
        0,
        pub,
        fr);

    ASSERT_TRUE(pub.last_bbo.has_value());
    EXPECT_EQ(pub.last_bbo->instrument_id, 200ULL);
    EXPECT_DOUBLE_EQ(pub.last_bbo->bid_price, 29990.0);
}

// ── Hyperliquid ───────────────────────────────────────────────────────────────

TEST(AdapterSmokeTest, HyperliquidL2Book) {
    adapter::SubscriptionMap subs;
    subs.subscribe(300, "BTC");
    adapter::HyperliquidMdDecoder<CapturePub> parser(subs);
    CapturePub pub;
    messaging::FundingRateCallback fr;

    parser.decode(
        R"({"channel":"l2Book","data":{"coin":"BTC","levels":[[{"px":"29990","sz":"1.5"}],[{"px":"29991","sz":"0.8"}]]}})",
        0,
        pub,
        fr);

    ASSERT_TRUE(pub.last_bbo.has_value());
    EXPECT_EQ(pub.last_bbo->instrument_id, 300ULL);
    EXPECT_DOUBLE_EQ(pub.last_bbo->bid_price, 29990.0);
}

}  // namespace
