// Unit tests for Hyperliquid action codec — pure JSON builders and the
// float_to_wire function that must match the HL Python SDK byte-for-byte.

#include "order_gateway/adapter/hyperliquid/hyperliquid_action_encoder.h"

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
// Asset metadata stubs — tests construct these inline rather than hitting
// HL's /info meta. Production path loads the map via
// parse_universe_meta from the live API response.
// ---------------------------------------------------------------------------

constexpr AssetMeta kBtcMeta{/*asset_idx=*/3, /*sz_decimals=*/5, /*max_px_decimals=*/1};
constexpr AssetMeta kEthMeta{/*asset_idx=*/4, /*sz_decimals=*/4, /*max_px_decimals=*/2};
constexpr AssetMeta kXmrMeta{/*asset_idx=*/202, /*sz_decimals=*/2, /*max_px_decimals=*/4};

// ---------------------------------------------------------------------------
// parse_universe_meta — HL /info meta response parsing
// ---------------------------------------------------------------------------

TEST(ParseUniverseMetaTest, BasicExtract) {
    // Stripped-down HL /info meta response. Asset index = position in array.
    constexpr const char* body = R"({
      "universe": [
        {"name": "SOL", "szDecimals": 2, "maxLeverage": 10},
        {"name": "APT", "szDecimals": 2, "maxLeverage": 3},
        {"name": "BTC", "szDecimals": 5, "maxLeverage": 40}
      ]
    })";
    const auto table = parse_universe_meta(body);
    ASSERT_EQ(table.size(), 3u);
    EXPECT_EQ(table.at("BTC").asset_idx, 2);
    EXPECT_EQ(table.at("BTC").sz_decimals, 5);
    EXPECT_EQ(table.at("BTC").max_px_decimals, 1);
    EXPECT_EQ(table.at("SOL").asset_idx, 0);
    EXPECT_EQ(table.at("APT").asset_idx, 1);
}

TEST(ParseUniverseMetaTest, ThrowsOnMissingUniverse) {
    EXPECT_THROW(parse_universe_meta("{\"other\":42}"), std::runtime_error);
}

TEST(ParseUniverseMetaTest, SkipsMalformedEntries) {
    // Entry missing `name` should be skipped, not crash startup.
    constexpr const char* body = R"({
      "universe": [
        {"name":"BTC","szDecimals":5},
        {"szDecimals":2},
        {"name":"ETH","szDecimals":4}
      ]
    })";
    const auto table = parse_universe_meta(body);
    EXPECT_EQ(table.size(), 2u);
    EXPECT_EQ(table.at("BTC").asset_idx, 0);
    EXPECT_EQ(table.at("ETH").asset_idx, 2);  // index preserved across skip
}

// ---------------------------------------------------------------------------
// parse_spot_universe_meta — HL /info spotMeta response parsing
// ---------------------------------------------------------------------------

TEST(ParseSpotUniverseMetaTest, BasicExtract) {
    // Two canonical-shape spot pairs. universe[i].index drives asset_idx
    // (= 10000 + index). Base-token szDecimals drives lot/tick precision.
    constexpr const char* body = R"({
      "tokens": [
        {"name":"USDC","szDecimals":8,"weiDecimals":8,"index":0,"isCanonical":true},
        {"name":"PURR","szDecimals":0,"weiDecimals":5,"index":1,"isCanonical":true},
        {"name":"HFUN","szDecimals":2,"weiDecimals":8,"index":2,"isCanonical":true}
      ],
      "universe": [
        {"name":"PURR/USDC","tokens":[1,0],"index":0,"isCanonical":true},
        {"name":"HFUN/USDC","tokens":[2,0],"index":2,"isCanonical":true}
      ]
    })";
    const auto table = parse_spot_universe_meta(body);
    ASSERT_EQ(table.size(), 2u);

    const auto& purr = table.at("PURR/USDC");
    EXPECT_EQ(purr.asset_idx, 10000);    // 10000 + 0
    EXPECT_EQ(purr.sz_decimals, 0);      // PURR base szDecimals
    EXPECT_EQ(purr.max_px_decimals, 8);  // 8 - 0 (spot rule)

    const auto& hfun = table.at("HFUN/USDC");
    EXPECT_EQ(hfun.asset_idx, 10002);  // 10000 + 2 — gap in index space
    EXPECT_EQ(hfun.sz_decimals, 2);
    EXPECT_EQ(hfun.max_px_decimals, 6);
}

TEST(ParseSpotUniverseMetaTest, IndexFieldNotArrayPosition) {
    // HL leaves index gaps when delisting; asset_idx must use the
    // `index` field, not the position in `universe[]`.
    constexpr const char* body = R"({
      "tokens": [
        {"name":"USDC","szDecimals":8,"index":0},
        {"name":"PURR","szDecimals":0,"index":1}
      ],
      "universe": [
        {"name":"PURR/USDC","tokens":[1,0],"index":7}
      ]
    })";
    const auto table = parse_spot_universe_meta(body);
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(table.at("PURR/USDC").asset_idx, 10007);  // 10000 + 7, NOT 10000
}

TEST(ParseSpotUniverseMetaTest, ThrowsOnMissingTokens) {
    EXPECT_THROW(parse_spot_universe_meta(R"({"universe":[]})"), std::runtime_error);
}

TEST(ParseSpotUniverseMetaTest, ThrowsOnMissingUniverse) {
    EXPECT_THROW(parse_spot_universe_meta(R"({"tokens":[]})"), std::runtime_error);
}

TEST(ParseSpotUniverseMetaTest, SkipsPairWithUnknownToken) {
    // universe references a token index not present in tokens[] →
    // pair is skipped, parsing continues for the rest.
    constexpr const char* body = R"({
      "tokens": [
        {"name":"USDC","szDecimals":8,"index":0},
        {"name":"PURR","szDecimals":0,"index":1}
      ],
      "universe": [
        {"name":"GHOST/USDC","tokens":[99,0],"index":0},
        {"name":"PURR/USDC","tokens":[1,0],"index":1}
      ]
    })";
    const auto table = parse_spot_universe_meta(body);
    EXPECT_EQ(table.size(), 1u);
    EXPECT_TRUE(table.contains("PURR/USDC"));
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
    auto action = build_order_action(kBtcMeta, true, 72198.5, 0.001, HlTif::Gtc);
    const auto& obj = action.as_object();

    EXPECT_EQ(obj.at("type").as_string(), "order");
    EXPECT_EQ(obj.at("grouping").as_string(), "na");

    const auto& orders = obj.at("orders").as_array();
    ASSERT_EQ(orders.size(), 1u);
    const auto& o = orders[0].as_object();

    EXPECT_EQ(o.at("a").as_int64(), 3);    // BTC asset_idx
    EXPECT_EQ(o.at("b").as_bool(), true);  // is_buy
    // Price rounded to integer for BTC at ~$72k — 5 sigfigs dominates.
    EXPECT_EQ(std::string(o.at("p").as_string()), "72199");
    EXPECT_EQ(std::string(o.at("s").as_string()), "0.001");
    EXPECT_EQ(o.at("r").as_bool(), false);

    const auto& tif = o.at("t").as_object().at("limit").as_object().at("tif").as_string();
    EXPECT_EQ(std::string(tif), "Gtc");
}

TEST(BuildOrderActionTest, SellOrderAlo) {
    auto action = build_order_action(kBtcMeta, false, 73000.0, 0.01, HlTif::Alo);
    const auto& o = action.as_object().at("orders").as_array()[0].as_object();

    EXPECT_EQ(o.at("b").as_bool(), false);
    const auto& tif = o.at("t").as_object().at("limit").as_object().at("tif").as_string();
    EXPECT_EQ(std::string(tif), "Alo");
}

TEST(BuildOrderActionTest, IocOrder) {
    auto action = build_order_action(kEthMeta, true, 3500.0, 0.5, HlTif::Ioc);
    const auto& o = action.as_object().at("orders").as_array()[0].as_object();

    EXPECT_EQ(o.at("a").as_int64(), 4);  // ETH asset_idx
    const auto& tif = o.at("t").as_object().at("limit").as_object().at("tif").as_string();
    EXPECT_EQ(std::string(tif), "Ioc");
}

TEST(BuildOrderActionTest, XmrPriceRounding) {
    // XMR at ~$385, szDecimals=2, max_px_decimals=4. 5-sigfig cap wins:
    // 385.123456 → 385.12 (2 decimals post-cap). Regression test for the
    // specific coin that prompted the /info-meta refactor.
    auto action = build_order_action(kXmrMeta, true, 385.123456, 0.1, HlTif::Gtc);
    const auto& o = action.as_object().at("orders").as_array()[0].as_object();
    EXPECT_EQ(o.at("a").as_int64(), 202);
    EXPECT_EQ(std::string(o.at("p").as_string()), "385.12");
}

// ---------------------------------------------------------------------------
// build_cancel_action
// ---------------------------------------------------------------------------

TEST(BuildCancelActionTest, Basic) {
    auto action = build_cancel_action(kBtcMeta, 12345);
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
    auto action = build_modify_action(kBtcMeta, 67890, 72000.0, 0.002);
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
