// Component tests for OKX L2 order book parsing — verifies that
// multi-level book snapshots are correctly parsed into MdOrderBook
// structs, which feed the OFI strategy's OFICalculator.

#include "fake_md_publisher.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/adapter/okx/okx_parser.h"

#include <gtest/gtest.h>

namespace bpt::md_gateway::adapter {
namespace {

struct OkxL2Fixture {
    SubscriptionMap subs;
    OkxParser parser{subs};
    test::FakeMdPublisher pub;
    messaging::FundingRateCallback fr_cb;

    void inject(const char* msg, uint64_t recv_ns = 0) { parser.parse(msg, recv_ns, pub, fr_cb); }
};

// ---------------------------------------------------------------------------
// Multi-level book
// ---------------------------------------------------------------------------

TEST(OkxL2BookTest, ThreeLevelBook) {
    OkxL2Fixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP", 5);

    f.inject(
        R"({"arg":{"channel":"books5","instId":"BTC-USDT-SWAP"},"data":[{
            "bids":[["29990","1.5","0","1"],["29989","2.0","0","2"],["29988","3.0","0","1"]],
            "asks":[["29991","0.8","0","1"],["29992","1.2","0","3"],["29993","0.5","0","1"]]
        }]})",
        111ULL);

    // Should publish both BBO and order book
    ASSERT_TRUE(f.pub.last_bbo.has_value());
    ASSERT_TRUE(f.pub.last_order_book.has_value());

    const auto& book = *f.pub.last_order_book;
    EXPECT_EQ(book.instrument_id, 1001ULL);
    EXPECT_EQ(book.timestamp_ns, 111ULL);

    ASSERT_EQ(book.bids.size(), 3u);
    EXPECT_DOUBLE_EQ(book.bids[0].first, 29990.0);
    EXPECT_DOUBLE_EQ(book.bids[0].second, 1.5);
    EXPECT_DOUBLE_EQ(book.bids[1].first, 29989.0);
    EXPECT_DOUBLE_EQ(book.bids[1].second, 2.0);
    EXPECT_DOUBLE_EQ(book.bids[2].first, 29988.0);
    EXPECT_DOUBLE_EQ(book.bids[2].second, 3.0);

    ASSERT_EQ(book.asks.size(), 3u);
    EXPECT_DOUBLE_EQ(book.asks[0].first, 29991.0);
    EXPECT_DOUBLE_EQ(book.asks[0].second, 0.8);
    EXPECT_DOUBLE_EQ(book.asks[1].first, 29992.0);
    EXPECT_DOUBLE_EQ(book.asks[1].second, 1.2);
    EXPECT_DOUBLE_EQ(book.asks[2].first, 29993.0);
    EXPECT_DOUBLE_EQ(book.asks[2].second, 0.5);
}

// ---------------------------------------------------------------------------
// Depth capping
// ---------------------------------------------------------------------------

TEST(OkxL2BookTest, DepthCappedBySubscription) {
    OkxL2Fixture f;
    // Subscribed with depth=2, even though exchange sends 5 levels
    f.subs.subscribe(1001, "BTC-USDT-SWAP", 2);

    f.inject(
        R"({"arg":{"channel":"books5","instId":"BTC-USDT-SWAP"},"data":[{
            "bids":[["29990","1.5","0","1"],["29989","2.0","0","2"],["29988","3.0","0","1"],["29987","4.0","0","1"],["29986","5.0","0","1"]],
            "asks":[["29991","0.8","0","1"],["29992","1.2","0","3"],["29993","0.5","0","1"],["29994","2.0","0","1"],["29995","3.0","0","1"]]
        }]})",
        222ULL);

    ASSERT_TRUE(f.pub.last_order_book.has_value());
    const auto& book = *f.pub.last_order_book;
    EXPECT_EQ(book.bids.size(), 2u);
    EXPECT_EQ(book.asks.size(), 2u);
}

// ---------------------------------------------------------------------------
// No order book when depth=0 (BBO only subscription)
// ---------------------------------------------------------------------------

TEST(OkxL2BookTest, NoOrderBookWhenDepthZero) {
    OkxL2Fixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP", 0);

    f.inject(
        R"({"arg":{"channel":"books5","instId":"BTC-USDT-SWAP"},"data":[{
            "bids":[["29990","1.5","0","1"]],
            "asks":[["29991","0.8","0","1"]]
        }]})",
        333ULL);

    // BBO should still publish
    ASSERT_TRUE(f.pub.last_bbo.has_value());
    // But no order book (depth=0 means BBO-only)
    EXPECT_FALSE(f.pub.last_order_book.has_value());
}

// ---------------------------------------------------------------------------
// Single level still publishes both BBO and book when depth >= 1
// ---------------------------------------------------------------------------

TEST(OkxL2BookTest, SingleLevelPublishesBothBboAndBook) {
    OkxL2Fixture f;
    f.subs.subscribe(1001, "BTC-USDT-SWAP", 5);

    f.inject(
        R"({"arg":{"channel":"books5","instId":"BTC-USDT-SWAP"},"data":[{
            "bids":[["29990","1.5","0","1"]],
            "asks":[["29991","0.8","0","1"]]
        }]})",
        444ULL);

    EXPECT_TRUE(f.pub.last_bbo.has_value());
    ASSERT_TRUE(f.pub.last_order_book.has_value());
    EXPECT_EQ(f.pub.last_order_book->bids.size(), 1u);
    EXPECT_EQ(f.pub.last_order_book->asks.size(), 1u);
}

}  // namespace
}  // namespace bpt::md_gateway::adapter
