#include "refdata/refdata/instrument.h"
#include "refdata/refdata/types.h"
#include "refdata/registry/instrument_registry.h"

#include <gtest/gtest.h>

using namespace bpt::refdata::registry;
using namespace bpt::refdata::refdata;

static Instrument make_instrument(uint64_t uid,
                                  const std::string& venue,
                                  const std::string& symbol,
                                  InstrumentType type) {
    Instrument inst{};
    inst.inst_uid = uid;
    inst.venue = venue;
    inst.venue_symbol = symbol;
    inst.display_name = symbol;
    inst.inst_type = type;
    inst.base = "BTC";
    inst.quote = "USDT";
    inst.tick_size = 0.01;
    inst.lot_size = 0.001;
    inst.contract_multiplier = 1.0;
    inst.status = InstrumentStatus::ACTIVE;
    inst.version = 1;
    return inst;
}

TEST(InstrumentRegistry, AddAndGetByUid) {
    InstrumentRegistry reg;
    auto inst = make_instrument(100101u, "BINANCE", "BTCUSDT", InstrumentType::PERP);
    reg.add(inst);

    auto result = reg.get(100101u);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->venue_symbol, "BTCUSDT");
    EXPECT_EQ(result->inst_uid, 100101u);
}

TEST(InstrumentRegistry, GetMissingUidReturnsNullopt) {
    InstrumentRegistry reg;
    EXPECT_FALSE(reg.get(999999u).has_value());
}

TEST(InstrumentRegistry, AddAndGetByVenueSymbol) {
    InstrumentRegistry reg;
    auto inst = make_instrument(100101u, "BINANCE", "BTCUSDT", InstrumentType::PERP);
    reg.add(inst);

    auto result = reg.get("BINANCE", "BTCUSDT");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->inst_uid, 100101u);
}

TEST(InstrumentRegistry, GetByVenueSymbolMissingReturnsNullopt) {
    InstrumentRegistry reg;
    EXPECT_FALSE(reg.get("BINANCE", "ETHUSDT").has_value());
}

TEST(InstrumentRegistry, AddAndGetByVenueSymbolType) {
    InstrumentRegistry reg;
    reg.add(make_instrument(100101u, "BINANCE", "BTCUSDT", InstrumentType::PERP));
    reg.add(make_instrument(200101u, "BINANCE", "BTCUSDT", InstrumentType::SPOT));

    auto perp = reg.get("BINANCE", "BTCUSDT", InstrumentType::PERP);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(perp->inst_uid, 100101u);
    EXPECT_EQ(perp->inst_type, InstrumentType::PERP);

    auto spot = reg.get("BINANCE", "BTCUSDT", InstrumentType::SPOT);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(spot->inst_uid, 200101u);
}

TEST(InstrumentRegistry, UpdateChangesFields) {
    InstrumentRegistry reg;
    auto inst = make_instrument(100101u, "BINANCE", "BTCUSDT", InstrumentType::PERP);
    reg.add(inst);

    auto updated = inst;
    updated.tick_size = 0.1;
    updated.version = 2;
    reg.update(updated);

    auto result = reg.get(100101u);
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->tick_size, 0.1);
    EXPECT_EQ(result->version, 2u);
}

TEST(InstrumentRegistry, Remove) {
    InstrumentRegistry reg;
    reg.add(make_instrument(100101u, "BINANCE", "BTCUSDT", InstrumentType::PERP));
    reg.remove(100101u);

    EXPECT_FALSE(reg.get(100101u).has_value());
    EXPECT_FALSE(reg.get("BINANCE", "BTCUSDT").has_value());
}

TEST(InstrumentRegistry, GetAllReturnsAll) {
    InstrumentRegistry reg;
    reg.add(make_instrument(100101u, "BINANCE", "BTCUSDT", InstrumentType::PERP));
    reg.add(make_instrument(100102u, "OKX", "BTC-USDT-SWAP", InstrumentType::PERP));
    reg.add(make_instrument(200101u, "BINANCE", "BTCUSDT", InstrumentType::SPOT));

    auto all = reg.getAll();
    EXPECT_EQ(all.size(), 3u);
}

TEST(InstrumentRegistry, GetAllEmptyRegistry) {
    InstrumentRegistry reg;
    EXPECT_TRUE(reg.getAll().empty());
}

TEST(InstrumentRegistry, AddIsIdempotentOnSameUid) {
    // Re-adding the same uid should behave like an update (registry upserts by uid)
    InstrumentRegistry reg;
    auto inst = make_instrument(100101u, "BINANCE", "BTCUSDT", InstrumentType::PERP);
    reg.add(inst);

    auto updated = inst;
    updated.tick_size = 0.5;
    reg.add(updated);

    EXPECT_EQ(reg.getAll().size(), 1u);
}
