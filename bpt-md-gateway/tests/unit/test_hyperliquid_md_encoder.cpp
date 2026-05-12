/// \file
/// \brief Verifies HL subscribe-payload wire format for both perp + spot symbols.
///
/// Empirical probe of the testnet WS (2026-05-11) confirmed that HL
/// accepts the pair name verbatim as the `coin` field for spot pairs —
/// e.g. `{"type":"l2Book","coin":"PURR/USDC"}` — and rejects the `@N`
/// universe-index notation (server closes the connection). The adapter's
/// existing symbol-agnostic path therefore handles spot without any
/// translation layer. These tests pin that contract.

#include "md_gateway/adapter/hyperliquid/hyperliquid_md_encoder.h"

#include <gtest/gtest.h>

using bpt::md_gateway::adapter::hyperliquid::build_ping_payload;
using bpt::md_gateway::adapter::hyperliquid::build_subscribe_payload;

TEST(HyperliquidMdEncoder, SubscribePayloadPerpSymbol) {
    const std::string payload = build_subscribe_payload("l2Book", "BTC");
    EXPECT_EQ(payload, R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC"}})");
}

TEST(HyperliquidMdEncoder, SubscribePayloadSpotPairName) {
    // Slash in the coin field is intentional — HL spot subscribe accepts
    // "BASE/QUOTE" verbatim and responds with frames whose `coin` field
    // mirrors the same string. SubscriptionMap.find_id sees "PURR/USDC"
    // both on subscribe and on every frame; no aliasing needed.
    const std::string payload = build_subscribe_payload("l2Book", "PURR/USDC");
    EXPECT_EQ(payload, R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"PURR/USDC"}})");
}

TEST(HyperliquidMdEncoder, SubscribePayloadTrades) {
    const std::string payload = build_subscribe_payload("trades", "PURR/USDC");
    EXPECT_EQ(payload, R"({"method":"subscribe","subscription":{"type":"trades","coin":"PURR/USDC"}})");
}

TEST(HyperliquidMdEncoder, PingPayloadIsAppLevel) {
    EXPECT_EQ(build_ping_payload(), R"({"method":"ping"})");
}
