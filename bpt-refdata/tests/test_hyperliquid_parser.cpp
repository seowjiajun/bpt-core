#include "refdata/adapter/hyperliquid/hyperliquid_parser.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/refdata/types.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <cmath>
#include <cstdio>
#include <gtest/gtest.h>
#include <memory>

using namespace bpt::refdata::adapter;
using namespace bpt::refdata::mapping;
using namespace bpt::refdata::refdata;

// ---------------------------------------------------------------------------
// Shared instrument mapping fixture
// ---------------------------------------------------------------------------

static const char* kMappingJson = R"({
  "exported_at": 1000000000000000000,
  "instrument_count": 2,
  "forward": {
    "3_BTC": 1001,
    "3_ETH": 1002
  },
  "reverse": {
    "1001": { "base":"BTC","quote":"USDT","type":"PERP","exchanges":{"3":"BTC"} },
    "1002": { "base":"ETH","quote":"USDT","type":"PERP","exchanges":{"3":"ETH"} }
  }
})";

static std::shared_ptr<InstrumentMappingLoader> make_mapping() {
    char path[] = "/tmp/test_hl_mapping_XXXXXX";
    int fd = mkstemp(path);
    write(fd, kMappingJson, strlen(kMappingJson));
    close(fd);
    auto loader = std::make_shared<InstrumentMappingLoader>();
    loader->load(path);
    std::remove(path);
    return loader;
}

// ---------------------------------------------------------------------------
// parse_meta
// ---------------------------------------------------------------------------

static const char* kMeta = R"({
  "universe": [
    {"name": "BTC", "szDecimals": 5, "maxLeverage": 50},
    {"name": "ETH", "szDecimals": 4, "maxLeverage": 25},
    {"name": "XYZ", "szDecimals": 3, "maxLeverage": 10}
  ]
})";

TEST(HyperliquidParser, MetaParsesKnownInstruments) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_meta(kMeta, 99u);

    // XYZ not in mapping → skipped; 2 results
    ASSERT_EQ(result.size(), 2u);
}

TEST(HyperliquidParser, MetaFieldsCorrect) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_meta(kMeta, 88u);
    ASSERT_GE(result.size(), 1u);

    auto btc = result[0];
    EXPECT_EQ(btc.venue, "HYPERLIQUID");
    EXPECT_EQ(btc.venue_symbol, "BTC");
    EXPECT_EQ(btc.base, "BTC");
    EXPECT_EQ(btc.quote, "USD");
    EXPECT_EQ(btc.inst_type, InstrumentType::PERP);
    EXPECT_EQ(btc.status, InstrumentStatus::ACTIVE);
    EXPECT_DOUBLE_EQ(btc.contract_multiplier, 1.0);
    EXPECT_EQ(btc.version, 88u);
    EXPECT_EQ(btc.display_name, "BTC-USD");
    EXPECT_EQ(btc.inst_uid, make_inst_uid(1001, EXCHANGE_ID_HYPERLIQUID));
}

TEST(HyperliquidParser, MetaLotSizeFromSzDecimals) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_meta(kMeta, 0u);
    ASSERT_GE(result.size(), 2u);

    // BTC szDecimals=5 → lot_size = 10^-5 = 0.00001
    EXPECT_DOUBLE_EQ(result[0].lot_size, std::pow(10.0, -5));
    // ETH szDecimals=4 → lot_size = 10^-4 = 0.0001
    EXPECT_DOUBLE_EQ(result[1].lot_size, std::pow(10.0, -4));
}

TEST(HyperliquidParser, MetaSkipsUnknownSymbol) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_meta(kMeta, 0u);
    for (const auto& inst : result)
        EXPECT_NE(inst.venue_symbol, "XYZ");
}

TEST(HyperliquidParser, MetaEmptyUniverseReturnsEmpty) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_meta(R"({"universe":[]})", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(HyperliquidParser, MetaMissingUniverseKeyReturnsEmpty) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_meta(R"({})", 0u);
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// parse_user_fees
// ---------------------------------------------------------------------------

static const char* kUserFees = R"({
  "feeSchedule": {
    "maker": "-0.0002",
    "taker": "0.0005"
  }
})";

TEST(HyperliquidParser, UserFeesParsedAsBps) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_user_fees(kUserFees, 77u);
    ASSERT_EQ(result.size(), 1u);

    const auto& fs = result[0];
    EXPECT_EQ(fs.exchange_id, bpt::messages::ExchangeId::HYPERLIQUID);
    EXPECT_EQ(fs.instrument_id, 0u);  // 0 = exchange-wide
    EXPECT_EQ(fs.instrument_type, bpt::messages::InstrumentType::PERPETUAL);
    EXPECT_EQ(fs.maker_fee_bps, -2);  // rebate
    EXPECT_EQ(fs.taker_fee_bps, 5);
    EXPECT_EQ(fs.updated_ts, 77u);
}

TEST(HyperliquidParser, UserFeesMissingScheduleReturnsEmpty) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_user_fees(R"({"dailyUserVlm":[]})", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(HyperliquidParser, UserFeesNullScheduleReturnsEmpty) {
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_user_fees(R"({"feeSchedule": null})", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(HyperliquidParser, UserFeesMakerRebateNegativeBps) {
    // Maker rebate should be negative bps
    HyperliquidParser parser(make_mapping());
    auto result = parser.parse_user_fees(kUserFees, 0u);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_LT(result[0].maker_fee_bps, 0);
}
