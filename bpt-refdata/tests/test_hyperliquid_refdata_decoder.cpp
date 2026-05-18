#include "refdata/adapter/hyperliquid/hyperliquid_refdata_decoder.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/model/types.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <cmath>
#include <cstdio>
#include <gtest/gtest.h>
#include <memory>

using bpt::refdata::adapter::HyperliquidRefdataDecoder;
using bpt::refdata::mapping::InstrumentMappingLoader;
using bpt::refdata::mapping::EXCHANGE_ID_HYPERLIQUID;
using bpt::refdata::mapping::make_inst_uid;
using bpt::refdata::model::InstrumentStatus;
using bpt::refdata::model::InstrumentType;

// ---------------------------------------------------------------------------
// Shared instrument mapping fixture
// ---------------------------------------------------------------------------

static const char* kMappingJson = R"({
  "exported_at": 1000000000000000000,
  "instrument_count": 3,
  "forward": {
    "3_BTC": 1001,
    "3_ETH": 1002,
    "3_PURR/USDC": 1500
  },
  "reverse": {
    "1001": { "base":"BTC","quote":"USDT","type":"PERP","exchanges":{"3":"BTC"} },
    "1002": { "base":"ETH","quote":"USDT","type":"PERP","exchanges":{"3":"ETH"} },
    "1500": { "base":"PURR","quote":"USDC","type":"SPOT","exchanges":{"3":"PURR/USDC"} }
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

TEST(HyperliquidRefdataDecoder, MetaParsesKnownInstruments) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_meta(kMeta, 99u);

    // XYZ not in mapping → skipped; 2 results
    ASSERT_EQ(result.size(), 2u);
}

TEST(HyperliquidRefdataDecoder, MetaFieldsCorrect) {
    HyperliquidRefdataDecoder parser(make_mapping());
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

TEST(HyperliquidRefdataDecoder, MetaLotSizeFromSzDecimals) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_meta(kMeta, 0u);
    ASSERT_GE(result.size(), 2u);

    // BTC szDecimals=5 → lot_size = 10^-5 = 0.00001
    EXPECT_DOUBLE_EQ(result[0].lot_size, std::pow(10.0, -5));
    // ETH szDecimals=4 → lot_size = 10^-4 = 0.0001
    EXPECT_DOUBLE_EQ(result[1].lot_size, std::pow(10.0, -4));
}

TEST(HyperliquidRefdataDecoder, MetaSkipsUnknownSymbol) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_meta(kMeta, 0u);
    for (const auto& inst : result)
        EXPECT_NE(inst.venue_symbol, "XYZ");
}

TEST(HyperliquidRefdataDecoder, MetaEmptyUniverseReturnsEmpty) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_meta(R"({"universe":[]})", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(HyperliquidRefdataDecoder, MetaMissingUniverseKeyReturnsEmpty) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_meta(R"({})", 0u);
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// parse_spot_meta
// ---------------------------------------------------------------------------

// Mirrors the shape returned by HL `POST /info {"type":"spotMeta"}`:
//   tokens[]  — per-token info indexed by `index`
//   universe[] — pair entries that reference tokens by index
static const char* kSpotMeta = R"({
  "tokens": [
    {"name":"USDC","szDecimals":8,"weiDecimals":8,"index":0,"isCanonical":true},
    {"name":"PURR","szDecimals":0,"weiDecimals":5,"index":1,"isCanonical":true},
    {"name":"TEST","szDecimals":1,"weiDecimals":8,"index":2,"isCanonical":false}
  ],
  "universe": [
    {"name":"PURR/USDC","tokens":[1,0],"index":0,"isCanonical":true},
    {"name":"@1","tokens":[2,0],"index":1,"isCanonical":false}
  ]
})";

TEST(HyperliquidRefdataDecoder, SpotMetaParsesKnownPairs) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_meta(kSpotMeta, 42u);
    // @1 (TEST/USDC) has no mapping entry → filtered. Only PURR/USDC survives.
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].venue_symbol, "PURR/USDC");
}

TEST(HyperliquidRefdataDecoder, SpotMetaFieldsCorrect) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_meta(kSpotMeta, 99u);
    ASSERT_EQ(result.size(), 1u);

    const auto& purr = result[0];
    EXPECT_EQ(purr.venue, "HYPERLIQUID");
    EXPECT_EQ(purr.venue_symbol, "PURR/USDC");
    EXPECT_EQ(purr.base, "PURR");
    EXPECT_EQ(purr.quote, "USDC");  // distinct from perp's hardcoded "USD"
    EXPECT_EQ(purr.inst_type, InstrumentType::SPOT);
    EXPECT_EQ(purr.status, InstrumentStatus::ACTIVE);
    EXPECT_DOUBLE_EQ(purr.contract_multiplier, 1.0);
    EXPECT_EQ(purr.version, 99u);
    EXPECT_EQ(purr.display_name, "PURR/USDC");
    EXPECT_EQ(purr.inst_uid, make_inst_uid(1500, EXCHANGE_ID_HYPERLIQUID));
}

TEST(HyperliquidRefdataDecoder, SpotMetaTickAndLotForSzDecimalsZero) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_meta(kSpotMeta, 0u);
    ASSERT_EQ(result.size(), 1u);

    // PURR szDecimals=0 → lot_size = 10^0 = 1.0
    EXPECT_DOUBLE_EQ(result[0].lot_size, 1.0);
    // Spot MAX_DECIMALS=8 (vs perp's 6); px_decimals = 8 - 0 = 8 → tick = 1e-8
    EXPECT_DOUBLE_EQ(result[0].tick_size, std::pow(10.0, -8));
}

TEST(HyperliquidRefdataDecoder, SpotMetaSkipsUnmappedPair) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_meta(kSpotMeta, 0u);
    for (const auto& inst : result)
        EXPECT_NE(inst.venue_symbol, "@1");
}

TEST(HyperliquidRefdataDecoder, SpotMetaEmptyUniverseReturnsEmpty) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_meta(R"({"tokens":[],"universe":[]})", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(HyperliquidRefdataDecoder, SpotMetaMissingKeysReturnsEmpty) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_meta(R"({})", 0u);
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

TEST(HyperliquidRefdataDecoder, UserFeesParsedAsBps) {
    HyperliquidRefdataDecoder parser(make_mapping());
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

TEST(HyperliquidRefdataDecoder, UserFeesMissingScheduleReturnsEmpty) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_user_fees(R"({"dailyUserVlm":[]})", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(HyperliquidRefdataDecoder, UserFeesNullScheduleReturnsEmpty) {
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_user_fees(R"({"feeSchedule": null})", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(HyperliquidRefdataDecoder, UserFeesMakerRebateNegativeBps) {
    // Maker rebate should be negative bps
    HyperliquidRefdataDecoder parser(make_mapping());
    auto result = parser.parse_user_fees(kUserFees, 0u);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_LT(result[0].maker_fee_bps, 0);
}
