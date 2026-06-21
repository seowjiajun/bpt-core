#include "strategy/strategy/canonical_resolver.h"

#include <gtest/gtest.h>

namespace {

using bpt::strategy::refdata::IRefdataClient;
using bpt::strategy::strategy::CanonicalResolver;
namespace messages = bpt::messages;

// ─── parse() ────────────────────────────────────────────────────────────────

TEST(CanonicalResolverParse, AcceptsSpot) {
    auto p = CanonicalResolver::parse("BTC/USDT:SPOT");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->base, "BTC");
    EXPECT_EQ(p->quote, "USDT");
}

TEST(CanonicalResolverParse, PerpAndPerpetualAliases) {
    auto a = CanonicalResolver::parse("ETH/USDT:PERP");
    auto b = CanonicalResolver::parse("ETH/USDT:PERPETUAL");
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->type, b->type);
}

TEST(CanonicalResolverParse, MalformedReturnsNullopt) {
    EXPECT_FALSE(CanonicalResolver::parse("BTC-USDT-SPOT"));  // no slash, no colon
    EXPECT_FALSE(CanonicalResolver::parse("BTC/USDT"));       // no colon
    EXPECT_FALSE(CanonicalResolver::parse("BTC:USDT/SPOT"));  // colon before slash
}

// ─── build_filters() ────────────────────────────────────────────────────────

TEST(CanonicalResolverBuildFilters, EmptySymbolsYieldsEmpty) {
    EXPECT_TRUE(CanonicalResolver::build_filters({}, {"BINANCE"}).empty());
}

TEST(CanonicalResolverBuildFilters, EmptyExchangesEmitsAnyExchangeFilter) {
    auto out = CanonicalResolver::build_filters({"BTC/USDT:SPOT"}, {});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].base, "BTC");
    EXPECT_EQ(out[0].quote, "USDT");
    EXPECT_EQ(out[0].instrument_type, messages::InstrumentType::SPOT);
    EXPECT_EQ(out[0].exchange, "");
}

TEST(CanonicalResolverBuildFilters, FansOutAcrossExchanges) {
    auto out = CanonicalResolver::build_filters({"BTC/USDT:PERP"}, {"BINANCE", "OKX", "HYPERLIQUID"});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].exchange, "BINANCE");
    EXPECT_EQ(out[1].exchange, "OKX");
    EXPECT_EQ(out[2].exchange, "HYPERLIQUID");
    for (const auto& f : out) {
        EXPECT_EQ(f.base, "BTC");
        EXPECT_EQ(f.quote, "USDT");
        EXPECT_EQ(f.instrument_type, messages::InstrumentType::PERPETUAL);
    }
}

TEST(CanonicalResolverBuildFilters, CrossesSymbolsAndExchanges) {
    auto out = CanonicalResolver::build_filters({"BTC/USDT:SPOT", "ETH/USDT:PERP"}, {"BINANCE", "OKX"});
    EXPECT_EQ(out.size(), 4u);  // 2 symbols × 2 exchanges
}

TEST(CanonicalResolverBuildFilters, SkipsMalformedSymbols) {
    auto out = CanonicalResolver::build_filters({"BAD", "BTC/USDT:SPOT", "WORSE-STILL"}, {"OKX"});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].base, "BTC");
}

TEST(CanonicalResolverBuildFilters, MapsAllInstrumentTypes) {
    auto spot = CanonicalResolver::build_filters({"BTC/USDT:SPOT"}, {});
    auto perp = CanonicalResolver::build_filters({"BTC/USDT:PERP"}, {});
    auto fut = CanonicalResolver::build_filters({"BTC/USDT:FUT"}, {});
    auto opt = CanonicalResolver::build_filters({"BTC/USDT:OPT"}, {});
    ASSERT_EQ(spot.size(), 1u);
    ASSERT_EQ(perp.size(), 1u);
    ASSERT_EQ(fut.size(), 1u);
    ASSERT_EQ(opt.size(), 1u);
    EXPECT_EQ(spot[0].instrument_type, messages::InstrumentType::SPOT);
    EXPECT_EQ(perp[0].instrument_type, messages::InstrumentType::PERPETUAL);
    EXPECT_EQ(fut[0].instrument_type, messages::InstrumentType::FUTURE);
    EXPECT_EQ(opt[0].instrument_type, messages::InstrumentType::OPTION);
}

// ─── matches() ──────────────────────────────────────────────────────────────

using bpt::strategy::refdata::Instrument;
using bpt::strategy::refdata::InstrumentType;

static Instrument make_inst(std::string base, std::string quote, InstrumentType type, std::string exchange) {
    Instrument i;
    i.base_currency = std::move(base);
    i.quote_currency = std::move(quote);
    i.type = type;
    i.exchange = std::move(exchange);
    return i;
}

TEST(CanonicalResolverMatches, EmptyListsMatchAnything) {
    EXPECT_TRUE(CanonicalResolver::matches({}, {}, make_inst("BTC", "USDT", InstrumentType::SPOT, "OKX")));
}

TEST(CanonicalResolverMatches, ExchangeFilterRejects) {
    auto i = make_inst("BTC", "USDT", InstrumentType::SPOT, "OKX");
    EXPECT_FALSE(CanonicalResolver::matches({}, {"BINANCE"}, i));
    EXPECT_TRUE(CanonicalResolver::matches({}, {"BINANCE", "OKX"}, i));
}

TEST(CanonicalResolverMatches, SymbolFilterMatchesBaseQuoteType) {
    auto i = make_inst("BTC", "USDT", InstrumentType::PERPETUAL, "OKX");
    EXPECT_TRUE(CanonicalResolver::matches({"BTC/USDT:PERP"}, {}, i));
    EXPECT_FALSE(CanonicalResolver::matches({"BTC/USDT:SPOT"}, {}, i));  // type mismatch
    EXPECT_FALSE(CanonicalResolver::matches({"ETH/USDT:PERP"}, {}, i));  // base mismatch
}

TEST(CanonicalResolverMatches, BothFiltersApplied) {
    auto i = make_inst("BTC", "USDT", InstrumentType::SPOT, "OKX");
    EXPECT_TRUE(CanonicalResolver::matches({"BTC/USDT:SPOT"}, {"OKX"}, i));
    EXPECT_FALSE(CanonicalResolver::matches({"BTC/USDT:SPOT"}, {"BINANCE"}, i));
}

}  // namespace
