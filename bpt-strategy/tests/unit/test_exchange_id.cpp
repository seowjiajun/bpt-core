#include "strategy/refdata/exchange_id.h"

#include <gtest/gtest.h>

namespace {

using bpt::messages::ExchangeId;
using bpt::strategy::refdata::to_exchange_id;

TEST(ToExchangeId, MapsKnownNames) {
    EXPECT_EQ(to_exchange_id("BINANCE"), ExchangeId::BINANCE);
    EXPECT_EQ(to_exchange_id("OKX"), ExchangeId::OKX);
    EXPECT_EQ(to_exchange_id("HYPERLIQUID"), ExchangeId::HYPERLIQUID);
    EXPECT_EQ(to_exchange_id("DERIBIT"), ExchangeId::DERIBIT);
}

TEST(ToExchangeId, UnknownReturnsNullValue) {
    EXPECT_EQ(to_exchange_id(""), ExchangeId::NULL_VALUE);
    EXPECT_EQ(to_exchange_id("UNKNOWN_VENUE"), ExchangeId::NULL_VALUE);
    EXPECT_EQ(to_exchange_id("binance"), ExchangeId::NULL_VALUE);  // case-sensitive by design
}

}  // namespace
