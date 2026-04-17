// Unit tests for Hyperliquid action codec — pure JSON builders and the
// float_to_wire function that must match the HL Python SDK byte-for-byte.

#include "order_gateway/adapter/hyperliquid/hyperliquid_action_codec.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <string>

namespace {

namespace json = boost::json;
using namespace bpt::order_gateway::adapter::hyperliquid;

// ---------------------------------------------------------------------------
// float_to_wire — the load-bearing piece for HL signature compatibility
// ---------------------------------------------------------------------------

TEST(FloatToWireTest, Integer) {
    EXPECT_EQ(float_to_wire(50000.0), "50000");
}

TEST(FloatToWireTest, TrailingZerosStripped) {
    EXPECT_EQ(float_to_wire(72198.05750000), "72198.0575");
}

TEST(FloatToWireTest, SmallDecimal) {
    EXPECT_EQ(float_to_wire(0.001), "0.001");
}

TEST(FloatToWireTest, Zero) {
    EXPECT_EQ(float_to_wire(0.0), "0");
}

TEST(FloatToWireTest, NegativeZero) {
    EXPECT_EQ(float_to_wire(-0.0), "0");
}

TEST(FloatToWireTest, LargeInteger) {
    EXPECT_EQ(float_to_wire(100000.0), "100000");
}

TEST(FloatToWireTest, OneDecimalPlace) {
    EXPECT_EQ(float_to_wire(72198.1), "72198.1");
}

TEST(FloatToWireTest, FullPrecision) {
    EXPECT_EQ(float_to_wire(0.12345678), "0.12345678");
}

// ---------------------------------------------------------------------------
// lookup_testnet_asset
// ---------------------------------------------------------------------------

TEST(AssetLookupTest, BTC) {
    auto meta = lookup_testnet_asset("BTC");
    EXPECT_EQ(meta.asset_idx, 3);
    EXPECT_EQ(meta.sz_decimals, 5);
    EXPECT_EQ(meta.max_px_decimals, 1);
}

TEST(AssetLookupTest, ETH) {
    auto meta = lookup_testnet_asset("ETH");
    EXPECT_EQ(meta.asset_idx, 4);
    EXPECT_EQ(meta.sz_decimals, 4);
    EXPECT_EQ(meta.max_px_decimals, 2);
}

TEST(AssetLookupTest, UnknownAsset) {
    auto meta = lookup_testnet_asset("SHIB");
    EXPECT_EQ(meta.asset_idx, -1);
}

// ---------------------------------------------------------------------------
// tif_to_string
// ---------------------------------------------------------------------------

TEST(TifTest, AllVariants) {
    EXPECT_STREQ(tif_to_string(HlTif::Gtc), "Gtc");
    EXPECT_STREQ(tif_to_string(HlTif::Alo), "Alo");
    EXPECT_STREQ(tif_to_string(HlTif::Ioc), "Ioc");
}

// ---------------------------------------------------------------------------
// build_order_action
// ---------------------------------------------------------------------------

TEST(BuildOrderActionTest, BasicBuyOrder) {
    auto action = build_order_action("BTC", true, 72198.5, 0.001, HlTif::Gtc);
    const auto& obj = action.as_object();

    EXPECT_EQ(obj.at("type").as_string(), "order");
    EXPECT_EQ(obj.at("grouping").as_string(), "na");

    const auto& orders = obj.at("orders").as_array();
    ASSERT_EQ(orders.size(), 1u);
    const auto& o = orders[0].as_object();

    EXPECT_EQ(o.at("a").as_int64(), 3);   // BTC asset_idx
    EXPECT_EQ(o.at("b").as_bool(), true);  // is_buy
    // Price rounded to integer for BTC at ~$72k
    EXPECT_EQ(std::string(o.at("p").as_string()), "72199");
    EXPECT_EQ(std::string(o.at("s").as_string()), "0.001");
    EXPECT_EQ(o.at("r").as_bool(), false);

    const auto& tif = o.at("t").as_object().at("limit").as_object().at("tif").as_string();
    EXPECT_EQ(std::string(tif), "Gtc");
}

TEST(BuildOrderActionTest, SellOrderAlo) {
    auto action = build_order_action("BTC", false, 73000.0, 0.01, HlTif::Alo);
    const auto& o = action.as_object().at("orders").as_array()[0].as_object();

    EXPECT_EQ(o.at("b").as_bool(), false);
    const auto& tif = o.at("t").as_object().at("limit").as_object().at("tif").as_string();
    EXPECT_EQ(std::string(tif), "Alo");
}

TEST(BuildOrderActionTest, IocOrder) {
    auto action = build_order_action("ETH", true, 3500.0, 0.5, HlTif::Ioc);
    const auto& o = action.as_object().at("orders").as_array()[0].as_object();

    EXPECT_EQ(o.at("a").as_int64(), 4);  // ETH asset_idx
    const auto& tif = o.at("t").as_object().at("limit").as_object().at("tif").as_string();
    EXPECT_EQ(std::string(tif), "Ioc");
}

// ---------------------------------------------------------------------------
// build_cancel_action
// ---------------------------------------------------------------------------

TEST(BuildCancelActionTest, Basic) {
    auto action = build_cancel_action("BTC", 12345);
    const auto& obj = action.as_object();

    EXPECT_EQ(obj.at("type").as_string(), "cancel");

    const auto& cancels = obj.at("cancels").as_array();
    ASSERT_EQ(cancels.size(), 1u);
    const auto& c = cancels[0].as_object();
    EXPECT_EQ(c.at("a").as_int64(), 3);
    EXPECT_EQ(c.at("o").as_uint64(), 12345u);
}

// ---------------------------------------------------------------------------
// build_modify_action
// ---------------------------------------------------------------------------

TEST(BuildModifyActionTest, Basic) {
    auto action = build_modify_action("BTC", 67890, 72000.0, 0.002);
    const auto& obj = action.as_object();

    EXPECT_EQ(obj.at("type").as_string(), "modify");
    const auto& mods = obj.at("modifies").as_array();
    ASSERT_EQ(mods.size(), 1u);
    const auto& m = mods[0].as_object();
    EXPECT_EQ(m.at("oid").as_uint64(), 67890u);

    const auto& order = m.at("order").as_object();
    EXPECT_EQ(std::string(order.at("p").as_string()), "72000");
    EXPECT_EQ(std::string(order.at("s").as_string()), "0.002");
}

// ---------------------------------------------------------------------------
// build_schedule_cancel_action
// ---------------------------------------------------------------------------

TEST(BuildScheduleCancelActionTest, WithTime) {
    auto action = build_schedule_cancel_action(1700000000000LL);
    const auto& obj = action.as_object();
    EXPECT_EQ(obj.at("type").as_string(), "scheduleCancel");
    EXPECT_EQ(obj.at("time").as_int64(), 1700000000000LL);
}

TEST(BuildScheduleCancelActionTest, ClearSchedule) {
    auto action = build_schedule_cancel_action(0);
    const auto& obj = action.as_object();
    EXPECT_EQ(obj.at("type").as_string(), "scheduleCancel");
    // time key should NOT be present when 0
    EXPECT_EQ(obj.find("time"), obj.end());
}

}  // namespace
