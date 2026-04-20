#include "refdata/adapter/okx/okx_parser.h"
#include "refdata/adapter/okx/okx_refdata_auth.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/refdata/types.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

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
  "instrument_count": 4,
  "forward": {
    "2_BTC-USDT-SWAP": 1001,
    "2_ETH-USDT-SWAP": 1002,
    "2_BTC-USDT":      2001,
    "2_ETH-USDT":      2002
  },
  "reverse": {
    "1001": { "base":"BTC","quote":"USDT","type":"PERP","exchanges":{"2":"BTC-USDT-SWAP"} },
    "1002": { "base":"ETH","quote":"USDT","type":"PERP","exchanges":{"2":"ETH-USDT-SWAP"} },
    "2001": { "base":"BTC","quote":"USDT","type":"SPOT","exchanges":{"2":"BTC-USDT"} },
    "2002": { "base":"ETH","quote":"USDT","type":"SPOT","exchanges":{"2":"ETH-USDT"} }
  }
})";

static std::shared_ptr<InstrumentMappingLoader> make_mapping() {
    char path[] = "/tmp/test_okx_mapping_XXXXXX";
    int fd = mkstemp(path);
    write(fd, kMappingJson, strlen(kMappingJson));
    close(fd);
    auto loader = std::make_shared<InstrumentMappingLoader>();
    loader->load(path);
    std::remove(path);
    return loader;
}

// ---------------------------------------------------------------------------
// parse_instruments — SWAP (perp)
// ---------------------------------------------------------------------------

static const char* kSwapInstruments = R"({
  "code": "0",
  "data": [
    {
      "instId": "BTC-USDT-SWAP", "instType": "SWAP", "state": "live",
      "baseCcy": "", "quoteCcy": "",
      "tickSz": "0.1", "lotSz": "0.001", "ctVal": "0.01"
    },
    {
      "instId": "ETH-USDT-SWAP", "instType": "SWAP", "state": "live",
      "baseCcy": "", "quoteCcy": "",
      "tickSz": "0.01", "lotSz": "0.01", "ctVal": "0.01"
    },
    {
      "instId": "XYZ-USDT-SWAP", "instType": "SWAP", "state": "live",
      "baseCcy": "", "quoteCcy": "",
      "tickSz": "0.01", "lotSz": "0.01", "ctVal": "1"
    },
    {
      "instId": "LTC-USDT-SWAP", "instType": "SWAP", "state": "suspend",
      "baseCcy": "", "quoteCcy": "",
      "tickSz": "0.01", "lotSz": "0.01", "ctVal": "1"
    }
  ]
})";

TEST(OKXParser, SwapParsesKnownInstruments) {
    OKXParser parser(make_mapping());
    auto result = parser.parse_instruments(kSwapInstruments, "SWAP", 55u);

    // XYZ not in mapping; LTC suspended → 2 results
    ASSERT_EQ(result.size(), 2u);

    auto btc = result[0];
    EXPECT_EQ(btc.venue, "OKX");
    EXPECT_EQ(btc.venue_symbol, "BTC-USDT-SWAP");
    EXPECT_EQ(btc.inst_type, InstrumentType::PERP);
    EXPECT_EQ(btc.status, InstrumentStatus::ACTIVE);
    EXPECT_DOUBLE_EQ(btc.tick_size, 0.1);
    // lot_size = lotSz * ctVal = 0.001 * 0.01 = 0.00001
    EXPECT_DOUBLE_EQ(btc.lot_size, 0.00001);
    EXPECT_DOUBLE_EQ(btc.contract_multiplier, 0.01);
    EXPECT_EQ(btc.version, 55u);
    EXPECT_EQ(btc.inst_uid, make_inst_uid(1001, EXCHANGE_ID_OKX));
}

TEST(OKXParser, SwapDerivesBaseQuoteFromInstId) {
    OKXParser parser(make_mapping());
    auto result = parser.parse_instruments(kSwapInstruments, "SWAP", 0u);
    ASSERT_GE(result.size(), 1u);

    // baseCcy/quoteCcy are empty in response; derived from "BTC-USDT-SWAP"
    EXPECT_EQ(result[0].base, "BTC");
    EXPECT_EQ(result[0].quote, "USDT");
}

TEST(OKXParser, SwapSkipsNonLiveState) {
    OKXParser parser(make_mapping());
    auto result = parser.parse_instruments(kSwapInstruments, "SWAP", 0u);
    for (const auto& inst : result)
        EXPECT_NE(inst.venue_symbol, "LTC-USDT-SWAP");
}

TEST(OKXParser, SwapSkipsUnknownMappingSymbol) {
    OKXParser parser(make_mapping());
    auto result = parser.parse_instruments(kSwapInstruments, "SWAP", 0u);
    for (const auto& inst : result)
        EXPECT_NE(inst.venue_symbol, "XYZ-USDT-SWAP");
}

// ---------------------------------------------------------------------------
// parse_instruments — SPOT
// ---------------------------------------------------------------------------

static const char* kSpotInstruments = R"({
  "code": "0",
  "data": [
    {
      "instId": "BTC-USDT", "instType": "SPOT", "state": "live",
      "baseCcy": "BTC", "quoteCcy": "USDT",
      "tickSz": "0.01", "lotSz": "0.00001", "ctVal": ""
    }
  ]
})";

TEST(OKXParser, SpotParsedCorrectly) {
    OKXParser parser(make_mapping());
    auto result = parser.parse_instruments(kSpotInstruments, "SPOT", 0u);
    ASSERT_EQ(result.size(), 1u);

    EXPECT_EQ(result[0].inst_type, InstrumentType::SPOT);
    EXPECT_EQ(result[0].base, "BTC");
    EXPECT_EQ(result[0].quote, "USDT");
    // ctVal empty → contract_multiplier stays 1.0; lot_size = lotSz * 1.0
    EXPECT_DOUBLE_EQ(result[0].contract_multiplier, 1.0);
    EXPECT_DOUBLE_EQ(result[0].lot_size, 0.00001);
    EXPECT_EQ(result[0].inst_uid, make_inst_uid(2001, EXCHANGE_ID_OKX));
}

// ---------------------------------------------------------------------------
// parse_instruments — error response
// ---------------------------------------------------------------------------

TEST(OKXParser, ErrorCodeReturnsEmpty) {
    OKXParser parser(make_mapping());
    auto result =
        parser.parse_instruments(R"({"code":"50001","msg":"Service temporarily unavailable","data":[]})", "SWAP", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(OKXParser, EmptyDataReturnsEmpty) {
    OKXParser parser(make_mapping());
    auto result = parser.parse_instruments(R"({"code":"0","data":[]})", "SWAP", 0u);
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// parse_trade_fee
// ---------------------------------------------------------------------------

static const char* kTradeFee = R"({
  "code": "0",
  "data": [
    {"instType": "SPOT", "maker": "-0.0008", "taker": "0.001",  "level": "Lv1"},
    {"instType": "SWAP", "maker": "-0.0002", "taker": "0.0005", "level": "Lv1"}
  ]
})";

TEST(OKXParser, TradeFeeParsedAsBps) {
    OKXParser parser(make_mapping());
    auto result = parser.parse_trade_fee(kTradeFee, 42u);
    ASSERT_EQ(result.size(), 2u);

    // SPOT
    EXPECT_EQ(result[0].exchange_id, bpt::messages::ExchangeId::OKX);
    EXPECT_EQ(result[0].instrument_id, 0u);  // 0 = exchange-wide
    EXPECT_EQ(result[0].instrument_type, bpt::messages::InstrumentType::SPOT);
    EXPECT_EQ(result[0].maker_fee_bps, -8);  // rebate
    EXPECT_EQ(result[0].taker_fee_bps, 10);
    EXPECT_EQ(result[0].updated_ts, 42u);

    // SWAP
    EXPECT_EQ(result[1].instrument_type, bpt::messages::InstrumentType::PERPETUAL);
    EXPECT_EQ(result[1].maker_fee_bps, -2);
    EXPECT_EQ(result[1].taker_fee_bps, 5);
}

TEST(OKXParser, TradeFeeErrorCodeReturnsEmpty) {
    OKXParser parser(make_mapping());
    auto result = parser.parse_trade_fee(R"({"code":"50013","msg":"Busy","data":[]})", 0u);
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// okx_auth_headers
// ---------------------------------------------------------------------------

TEST(OKXAuthHeaders, ContainsRequiredFields) {
    auto headers = okx_auth_headers("my-key", "my-secret", "my-pass", "GET", "/api/v5/account/trade-fee?instType=SPOT");

    std::unordered_map<std::string, std::string> hmap;
    for (const auto& [k, v] : headers)
        hmap[k] = v;

    EXPECT_EQ(hmap["OK-ACCESS-KEY"], "my-key");
    EXPECT_EQ(hmap["OK-ACCESS-PASSPHRASE"], "my-pass");
    EXPECT_FALSE(hmap["OK-ACCESS-SIGN"].empty());
    EXPECT_FALSE(hmap["OK-ACCESS-TIMESTAMP"].empty());
    EXPECT_EQ(hmap.count("x-simulated-trading"), 0u);  // not simulated
}

TEST(OKXAuthHeaders, SimulatedFlagAddsHeader) {
    auto headers = okx_auth_headers("key",
                                    "secret",
                                    "pass",
                                    "GET",
                                    "/api/v5/test",
                                    /*simulated=*/true);

    bool found = false;
    for (const auto& [k, v] : headers)
        if (k == "x-simulated-trading" && v == "1")
            found = true;
    EXPECT_TRUE(found);
}
