#include "refdata/adapter/binance/binance_refdata_decoder.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/model/types.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

using bpt::refdata::adapter::BinanceRefdataDecoder;
using bpt::refdata::mapping::InstrumentMappingLoader;
using bpt::refdata::mapping::EXCHANGE_ID_BINANCE;
using bpt::refdata::mapping::make_inst_uid;
using bpt::refdata::model::InstrumentStatus;
using bpt::refdata::model::InstrumentType;

// ---------------------------------------------------------------------------
// Shared instrument mapping fixture
// ---------------------------------------------------------------------------

static const char* kMappingJson = R"({
  "exported_at": 1000000000000000000,
  "instrument_count": 4,
  "forward": {
    "1_BTCUSDT":      1001,
    "1_ETHUSDT":      1002,
    "1_BTCUSDT_SPOT": 2001,
    "1_ETHUSDT_SPOT": 2002
  },
  "reverse": {
    "1001": { "base":"BTC","quote":"USDT","type":"PERP","exchanges":{"1":"BTCUSDT"} },
    "1002": { "base":"ETH","quote":"USDT","type":"PERP","exchanges":{"1":"ETHUSDT"} },
    "2001": { "base":"BTC","quote":"USDT","type":"SPOT","exchanges":{"1":"BTCUSDT"} },
    "2002": { "base":"ETH","quote":"USDT","type":"SPOT","exchanges":{"1":"ETHUSDT"} }
  }
})";

static std::shared_ptr<InstrumentMappingLoader> make_mapping() {
    char path[] = "/tmp/test_binance_mapping_XXXXXX";
    int fd = mkstemp(path);
    write(fd, kMappingJson, strlen(kMappingJson));
    close(fd);

    auto loader = std::make_shared<InstrumentMappingLoader>();
    loader->load(path);
    std::remove(path);
    return loader;
}

// ---------------------------------------------------------------------------
// parse_spot_exchange_info
// ---------------------------------------------------------------------------

static const char* kSpotExchangeInfo = R"({
  "symbols": [
    {
      "symbol": "BTCUSDT", "status": "TRADING",
      "baseAsset": "BTC", "quoteAsset": "USDT",
      "filters": [
        {"filterType": "PRICE_FILTER", "tickSize": "0.01"},
        {"filterType": "LOT_SIZE",     "stepSize": "0.00001"},
        {"filterType": "MIN_NOTIONAL", "minNotional": "10.0"}
      ]
    },
    {
      "symbol": "ETHUSDT", "status": "TRADING",
      "baseAsset": "ETH", "quoteAsset": "USDT",
      "filters": [
        {"filterType": "PRICE_FILTER", "tickSize": "0.001"},
        {"filterType": "LOT_SIZE",     "stepSize": "0.0001"}
      ]
    },
    {
      "symbol": "XYZUSDT", "status": "TRADING",
      "baseAsset": "XYZ", "quoteAsset": "USDT",
      "filters": []
    },
    {
      "symbol": "LTCUSDT", "status": "BREAK",
      "baseAsset": "LTC", "quoteAsset": "USDT",
      "filters": []
    }
  ]
})";

TEST(BinanceRefdataDecoder, SpotParsesKnownInstruments) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_exchange_info(kSpotExchangeInfo, 12345u);

    ASSERT_EQ(result.size(), 2u);  // XYZUSDT not in mapping; LTCUSDT not TRADING

    auto btc = result[0];
    EXPECT_EQ(btc.venue, "BINANCE");
    EXPECT_EQ(btc.venue_symbol, "BTCUSDT");
    EXPECT_EQ(btc.base, "BTC");
    EXPECT_EQ(btc.quote, "USDT");
    EXPECT_EQ(btc.inst_type, InstrumentType::SPOT);
    EXPECT_EQ(btc.status, InstrumentStatus::ACTIVE);
    EXPECT_DOUBLE_EQ(btc.tick_size, 0.01);
    EXPECT_DOUBLE_EQ(btc.lot_size, 0.00001);
    EXPECT_DOUBLE_EQ(btc.contract_multiplier, 1.0);
    EXPECT_EQ(btc.version, 12345u);
    EXPECT_EQ(btc.inst_uid, make_inst_uid(2001, EXCHANGE_ID_BINANCE));

    auto eth = result[1];
    EXPECT_EQ(eth.venue_symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(eth.tick_size, 0.001);
    EXPECT_DOUBLE_EQ(eth.lot_size, 0.0001);
}

TEST(BinanceRefdataDecoder, SpotSkipsUnknownMappingSymbol) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_exchange_info(kSpotExchangeInfo, 0u);
    for (const auto& inst : result)
        EXPECT_NE(inst.venue_symbol, "XYZUSDT");
}

TEST(BinanceRefdataDecoder, SpotSkipsNonTradingStatus) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_exchange_info(kSpotExchangeInfo, 0u);
    for (const auto& inst : result)
        EXPECT_NE(inst.venue_symbol, "LTCUSDT");
}

TEST(BinanceRefdataDecoder, SpotEmptySymbolsReturnsEmpty) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_spot_exchange_info(R"({"symbols":[]})", 0u);
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// parse_futures_exchange_info
// ---------------------------------------------------------------------------

static const char* kFuturesExchangeInfo = R"({
  "symbols": [
    {
      "symbol": "BTCUSDT", "status": "TRADING",
      "baseAsset": "BTC", "quoteAsset": "USDT",
      "contractType": "PERPETUAL", "contractSize": 1,
      "deliveryDate": 4133404800000,
      "filters": [
        {"filterType": "PRICE_FILTER", "tickSize": "0.10"},
        {"filterType": "LOT_SIZE",     "stepSize": "0.001"}
      ]
    },
    {
      "symbol": "ETHUSDT", "status": "TRADING",
      "baseAsset": "ETH", "quoteAsset": "USDT",
      "contractType": "PERPETUAL", "contractSize": 1,
      "deliveryDate": 4133404800000,
      "filters": [
        {"filterType": "PRICE_FILTER", "tickSize": "0.01"},
        {"filterType": "LOT_SIZE",     "stepSize": "0.01"}
      ]
    },
    {
      "symbol": "XYZUSDT", "status": "TRADING",
      "baseAsset": "XYZ", "quoteAsset": "USDT",
      "contractType": "PERPETUAL", "contractSize": 1,
      "deliveryDate": 4133404800000,
      "filters": []
    },
    {
      "symbol": "BTCUSDT_SETTLED", "status": "SETTLING",
      "baseAsset": "BTC", "quoteAsset": "USDT",
      "contractType": "CURRENT_QUARTER", "contractSize": 1,
      "deliveryDate": 1711670400000,
      "filters": []
    }
  ]
})";

TEST(BinanceRefdataDecoder, FuturesParsesPerp) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_futures_exchange_info(kFuturesExchangeInfo, 99u);

    // XYZUSDT not in mapping; BTCUSDT_SETTLED not TRADING/DELIVERING → 2 results
    ASSERT_EQ(result.size(), 2u);

    auto btc = result[0];
    EXPECT_EQ(btc.inst_type, InstrumentType::PERP);
    EXPECT_EQ(btc.status, InstrumentStatus::ACTIVE);
    EXPECT_DOUBLE_EQ(btc.tick_size, 0.10);
    EXPECT_DOUBLE_EQ(btc.lot_size, 0.001);
    EXPECT_DOUBLE_EQ(btc.contract_multiplier, 1.0);
    EXPECT_EQ(btc.inst_uid, make_inst_uid(1001, EXCHANGE_ID_BINANCE));
}

TEST(BinanceRefdataDecoder, FuturesDeliveryDateConvertedToNs) {
    // deliveryDate in ms — should be stored as ns
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_futures_exchange_info(kFuturesExchangeInfo, 0u);
    ASSERT_GE(result.size(), 1u);

    // BTCUSDT: deliveryDate = 4133404800000 ms
    ASSERT_TRUE(result[0].expiry_timestamp.has_value());
    EXPECT_EQ(*result[0].expiry_timestamp, 4133404800000ULL * 1'000'000ULL);
}

TEST(BinanceRefdataDecoder, FuturesSkipsNonTradingStatus) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_futures_exchange_info(kFuturesExchangeInfo, 0u);
    for (const auto& inst : result)
        EXPECT_NE(inst.venue_symbol, "BTCUSDT_SETTLED");
}

// ---------------------------------------------------------------------------
// parse_trade_fee
// ---------------------------------------------------------------------------

static const char* kTradeFee = R"([
  {"symbol": "BTCUSDT", "makerCommission": "0.001",  "takerCommission": "0.001"},
  {"symbol": "ETHUSDT", "makerCommission": "0.0008", "takerCommission": "0.001"},
  {"symbol": "XYZUSDT", "makerCommission": "0.001",  "takerCommission": "0.001"}
])";

TEST(BinanceRefdataDecoder, TradeFeeParsedAsBps) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_trade_fee(kTradeFee, 777u);

    // XYZUSDT_SPOT not in mapping → skipped; 2 results
    ASSERT_EQ(result.size(), 2u);

    auto btc = result[0];
    EXPECT_EQ(btc.exchange_id, bpt::messages::ExchangeId::BINANCE);
    EXPECT_EQ(btc.instrument_type, bpt::messages::InstrumentType::SPOT);
    EXPECT_EQ(btc.maker_fee_bps, 10);
    EXPECT_EQ(btc.taker_fee_bps, 10);
    EXPECT_EQ(btc.updated_ts, 777u);
    EXPECT_EQ(btc.instrument_id, make_inst_uid(2001, EXCHANGE_ID_BINANCE));

    auto eth = result[1];
    EXPECT_EQ(eth.maker_fee_bps, 8);
    EXPECT_EQ(eth.taker_fee_bps, 10);
}

TEST(BinanceRefdataDecoder, TradeFeeSkipsUnknownMappingSymbol) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_trade_fee(kTradeFee, 0u);
    EXPECT_EQ(result.size(), 2u);  // XYZUSDT skipped
}

TEST(BinanceRefdataDecoder, TradeFeeEmptyArrayReturnsEmpty) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_trade_fee("[]", 0u);
    EXPECT_TRUE(result.empty());
}

TEST(BinanceRefdataDecoder, TradeFeeNonArrayReturnsEmpty) {
    BinanceRefdataDecoder parser(make_mapping());
    auto result = parser.parse_trade_fee(R"({"error":"unauthorized"})", 0u);
    EXPECT_TRUE(result.empty());
}
