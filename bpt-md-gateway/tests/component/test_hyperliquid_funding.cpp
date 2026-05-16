// Component tests for Hyperliquid funding rate parsing.
// Verifies the activeAssetCtx channel is correctly parsed into
// FundingRateUpdate structs.

#include "fake_md_publisher.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_decoder.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"

#include <messages/ExchangeId.h>

#include <gtest/gtest.h>
#include <optional>

namespace bpt::md_gateway::adapter {
namespace {

struct HLFundingFixture {
    SubscriptionMap subs;
    HyperliquidMdDecoder<test::FakeMdPublisher> parser{subs};
    test::FakeMdPublisher pub;
    std::optional<messaging::FundingRateUpdate> last_fr;
    messaging::FundingRateCallback fr_cb;
    messaging::InstrumentStatsCallback stats_cb;

    HLFundingFixture() {
        fr_cb = [this](const messaging::FundingRateUpdate& fr) {
            last_fr = fr;
        };
    }

    void inject(const char* msg, uint64_t recv_ns = 0) { parser.decode(msg, recv_ns, pub, fr_cb, stats_cb); }
};

// ---------------------------------------------------------------------------
// Basic funding rate
// ---------------------------------------------------------------------------

TEST(HyperliquidFundingTest, ActiveAssetCtxParsed) {
    HLFundingFixture f;
    f.subs.subscribe(1001, "BTC");

    f.inject(
        R"({"channel":"activeAssetCtx","data":{"coin":"BTC","ctx":{"funding":"0.0001","openInterest":"1234.5","prevDayPx":"70000","dayNtlVlm":"50000000","premium":"0.0002","oraclePx":"71000","markPx":"71001","midPx":"71000.5","impactPxs":["70999","71001"]}}})",
        555ULL);

    ASSERT_TRUE(f.last_fr.has_value());
    const auto& fr = *f.last_fr;
    EXPECT_EQ(fr.instrument_id, 1001ULL);
    EXPECT_EQ(fr.exchange_id, bpt::messages::ExchangeId::HYPERLIQUID);
    // rate = 0.0001, multiplied by 1e6 → 100
    EXPECT_EQ(fr.rate_bps, 100);
    EXPECT_EQ(fr.collected_ts_ns, 555ULL);
    EXPECT_EQ(fr.next_funding_ts_ns, 0ULL);
}

// ---------------------------------------------------------------------------
// Negative funding rate
// ---------------------------------------------------------------------------

TEST(HyperliquidFundingTest, NegativeFundingRate) {
    HLFundingFixture f;
    f.subs.subscribe(1001, "BTC");

    f.inject(
        R"({"channel":"activeAssetCtx","data":{"coin":"BTC","ctx":{"funding":"-0.00025","openInterest":"1000","prevDayPx":"70000","dayNtlVlm":"30000000","premium":"-0.001","oraclePx":"71000","markPx":"70990","midPx":"70995","impactPxs":["70990","71000"]}}})",
        666ULL);

    ASSERT_TRUE(f.last_fr.has_value());
    // -0.00025 * 1e6 = -250
    EXPECT_EQ(f.last_fr->rate_bps, -250);
}

// ---------------------------------------------------------------------------
// Unknown coin dropped
// ---------------------------------------------------------------------------

TEST(HyperliquidFundingTest, UnknownCoinDropped) {
    HLFundingFixture f;
    f.subs.subscribe(1001, "BTC");

    f.inject(
        R"({"channel":"activeAssetCtx","data":{"coin":"ETH","ctx":{"funding":"0.0001","openInterest":"500","prevDayPx":"3000","dayNtlVlm":"10000000","premium":"0.0001","oraclePx":"3100","markPx":"3101","midPx":"3100.5","impactPxs":["3099","3101"]}}})");

    EXPECT_FALSE(f.last_fr.has_value());
}

// ---------------------------------------------------------------------------
// No callback set — should not crash
// ---------------------------------------------------------------------------

TEST(HyperliquidFundingTest, NullCallbackSafe) {
    HLFundingFixture f;
    f.fr_cb = nullptr;  // clear the callback
    f.subs.subscribe(1001, "BTC");

    // Should not crash
    f.inject(
        R"({"channel":"activeAssetCtx","data":{"coin":"BTC","ctx":{"funding":"0.0001","openInterest":"1000","prevDayPx":"70000","dayNtlVlm":"30000000","premium":"0.0001","oraclePx":"71000","markPx":"71001","midPx":"71000.5","impactPxs":["70999","71001"]}}})");

    EXPECT_FALSE(f.last_fr.has_value());
}

}  // namespace
}  // namespace bpt::md_gateway::adapter
