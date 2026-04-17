#include "refdata/mapping/instrument_mapping_loader.h"

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>

using namespace bpt::refdata::mapping;

// ---------------------------------------------------------------------------
// Helper: write a minimal valid instrument_mapping.json to a temp file.
// ---------------------------------------------------------------------------

static std::string write_temp_mapping(const std::string& content) {
    char path[] = "/tmp/test_mapping_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        throw std::runtime_error("mkstemp failed");
    if (write(fd, content.data(), content.size()) < 0)
        throw std::runtime_error("write to temp file failed");
    close(fd);
    return path;
}

static const std::string kMinimalMapping = R"({
  "exported_at": 1000000000000000000,
  "instrument_count": 3,
  "forward": {
    "1_BTCUSDT":       1001,
    "2_BTC-USDT-SWAP": 1001,
    "3_BTC":           1001,
    "1_ETHUSDT":       1002,
    "2_ETH-USDT-SWAP": 1002,
    "3_ETH":           1002,
    "1_SOLUSDT":       1003,
    "2_SOL-USDT-SWAP": 1003,
    "3_SOL":           1003,
    "1_BTCUSDT_SPOT":  2001,
    "2_BTC-USDT":      2001,
    "1_ETHUSDT_SPOT":  2002,
    "2_ETH-USDT":      2002
  },
  "reverse": {
    "1001": {
      "base": "BTC", "quote": "USDT", "type": "PERP",
      "exchanges": { "1": "BTCUSDT", "2": "BTC-USDT-SWAP", "3": "BTC" }
    },
    "1002": {
      "base": "ETH", "quote": "USDT", "type": "PERP",
      "exchanges": { "1": "ETHUSDT", "2": "ETH-USDT-SWAP", "3": "ETH" }
    },
    "1003": {
      "base": "SOL", "quote": "USDT", "type": "PERP",
      "exchanges": { "1": "SOLUSDT", "2": "SOL-USDT-SWAP", "3": "SOL" }
    },
    "2001": {
      "base": "BTC", "quote": "USDT", "type": "SPOT",
      "exchanges": { "1": "BTCUSDT", "2": "BTC-USDT" }
    },
    "2002": {
      "base": "ETH", "quote": "USDT", "type": "SPOT",
      "exchanges": { "1": "ETHUSDT", "2": "ETH-USDT" }
    }
  }
})";

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(InstrumentMappingLoader, LoadsFileSuccessfully) {
    auto path = write_temp_mapping(kMinimalMapping);
    InstrumentMappingLoader loader;
    EXPECT_NO_THROW(loader.load(path));
    EXPECT_EQ(loader.instrument_count(), 5u);
    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, BTCPerpResolvesOnAllThreeExchanges) {
    auto path = write_temp_mapping(kMinimalMapping);
    InstrumentMappingLoader loader;
    loader.load(path);

    // Raw canonical ID returned by the resolver is 1001 on all exchanges
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_BINANCE, "BTCUSDT"), 1001u);
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_OKX, "BTC-USDT-SWAP"), 1001u);
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_HYPERLIQUID, "BTC"), 1001u);

    // make_inst_uid encodes (canonical_id, exchange_id) into a unique registry key
    EXPECT_EQ(make_inst_uid(1001, EXCHANGE_ID_BINANCE), 100101u);
    EXPECT_EQ(make_inst_uid(1001, EXCHANGE_ID_OKX), 100102u);
    EXPECT_EQ(make_inst_uid(1001, EXCHANGE_ID_HYPERLIQUID), 100103u);

    // canonical_id_from_uid recovers the canonical ID
    EXPECT_EQ(canonical_id_from_uid(100101u), 1001u);
    EXPECT_EQ(canonical_id_from_uid(100102u), 1001u);

    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, BTCSpotResolvesCorrectly) {
    auto path = write_temp_mapping(kMinimalMapping);
    InstrumentMappingLoader loader;
    loader.load(path);

    // Binance SPOT uses _SPOT suffix in forward key
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_BINANCE, "BTCUSDT_SPOT"), 2001u);
    // OKX SPOT (symbol is already distinct)
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_OKX, "BTC-USDT"), 2001u);

    // inst_uid for BTC SPOT on Binance
    EXPECT_EQ(make_inst_uid(2001, EXCHANGE_ID_BINANCE), 200101u);

    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, UnknownSymbolReturnNullopt) {
    auto path = write_temp_mapping(kMinimalMapping);
    InstrumentMappingLoader loader;
    loader.load(path);

    auto result = loader.try_resolve_canonical_id(EXCHANGE_ID_BINANCE, "XYZUSDT");
    EXPECT_FALSE(result.has_value());

    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, ResolveCanonicalIdLogsWarnOnMiss) {
    // resolve_canonical_id (targeted) returns UNKNOWN_INSTRUMENT on miss
    auto path = write_temp_mapping(kMinimalMapping);
    InstrumentMappingLoader loader;
    loader.load(path);

    EXPECT_EQ(loader.resolve_canonical_id(EXCHANGE_ID_BINANCE, "UNKNOWN"), InstrumentMappingLoader::UNKNOWN_INSTRUMENT);

    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, ResolveSymbolReturnsCorrectString) {
    auto path = write_temp_mapping(kMinimalMapping);
    InstrumentMappingLoader loader;
    loader.load(path);

    EXPECT_EQ(loader.resolve_symbol(1001, EXCHANGE_ID_BINANCE), "BTCUSDT");
    EXPECT_EQ(loader.resolve_symbol(1001, EXCHANGE_ID_OKX), "BTC-USDT-SWAP");
    EXPECT_EQ(loader.resolve_symbol(1001, EXCHANGE_ID_HYPERLIQUID), "BTC");
    EXPECT_EQ(loader.resolve_symbol(2001, EXCHANGE_ID_OKX), "BTC-USDT");

    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, ResolveSymbolReturnsEmptyOnMiss) {
    auto path = write_temp_mapping(kMinimalMapping);
    InstrumentMappingLoader loader;
    loader.load(path);

    // Hyperliquid has no SPOT instruments in this mapping
    EXPECT_EQ(loader.resolve_symbol(2001, EXCHANGE_ID_HYPERLIQUID), "");

    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, GetInstrumentInfoReturnsCorrectFields) {
    auto path = write_temp_mapping(kMinimalMapping);
    InstrumentMappingLoader loader;
    loader.load(path);

    auto info = loader.get_instrument_info(1001);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->base, "BTC");
    EXPECT_EQ(info->quote, "USDT");
    EXPECT_EQ(info->type, "PERP");

    auto spot_info = loader.get_instrument_info(2001);
    ASSERT_TRUE(spot_info.has_value());
    EXPECT_EQ(spot_info->type, "SPOT");

    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, MissingFileThrowsOnLoad) {
    InstrumentMappingLoader loader;
    EXPECT_THROW(loader.load("/nonexistent/path/instrument_mapping.json"), std::runtime_error);
}

TEST(InstrumentMappingLoader, MalformedJsonThrowsOnLoad) {
    auto path = write_temp_mapping("{ this is not json }");
    InstrumentMappingLoader loader;
    EXPECT_THROW(loader.load(path), std::runtime_error);
    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, MissingForwardKeyThrowsOnLoad) {
    auto path = write_temp_mapping(R"({"exported_at":1,"reverse":{}})");
    InstrumentMappingLoader loader;
    EXPECT_THROW(loader.load(path), std::runtime_error);
    std::remove(path.c_str());
}

TEST(InstrumentMappingLoader, ExplicitReloadPicksUpNewInstruments) {
    // Write initial mapping with only BTC
    std::string initial = R"({
      "exported_at": 1,
      "forward": { "1_BTCUSDT": 1001 },
      "reverse": {
        "1001": { "base":"BTC","quote":"USDT","type":"PERP","exchanges":{"1":"BTCUSDT"} }
      }
    })";
    auto path = write_temp_mapping(initial);
    InstrumentMappingLoader loader;
    loader.load(path);
    EXPECT_EQ(loader.instrument_count(), 1u);

    // Overwrite with updated mapping containing 2 instruments
    std::string updated = R"({
      "exported_at": 2,
      "forward": { "1_BTCUSDT": 1001, "1_ETHUSDT": 1002 },
      "reverse": {
        "1001": { "base":"BTC","quote":"USDT","type":"PERP","exchanges":{"1":"BTCUSDT"} },
        "1002": { "base":"ETH","quote":"USDT","type":"PERP","exchanges":{"1":"ETHUSDT"} }
      }
    })";
    {
        std::ofstream f(path, std::ios::trunc);
        f << updated;
    }

    // Caller drives refresh — call load() again after the file has been updated.
    loader.load(path);
    EXPECT_EQ(loader.instrument_count(), 2u);
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_BINANCE, "ETHUSDT"), 1002u);

    std::remove(path.c_str());
}
