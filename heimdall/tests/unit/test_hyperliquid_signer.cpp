// Verifies HyperliquidSigner byte-for-byte against a reference signature
// produced by the Python eth-account SDK (see /tmp/hl_ref/ref_sign.py).
//
// If this test fails, the C++ signer has drifted from Hyperliquid's wire
// protocol — do NOT ship until fixed. Each value below is checked in the
// same order the algorithm computes them, so the first mismatch points
// at the exact step that broke.

#include "heimdall/adapter/hyperliquid/hyperliquid_signer.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <string>

namespace {

namespace json = boost::json;
using heimdall::adapter::HyperliquidSigner;

// Fixed test vector from /tmp/hl_ref/ref_sign.py
//
// priv  = 0x1111...1111
// chain = testnet (source="b")
// nonce = 1700000000000
// action = {"type":"order","orders":[{"a":0,"b":true,"p":"50000.0","s":"0.001","r":false,"t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"}
TEST(HyperliquidSignerTest, MatchesPythonReference) {
    const char* kPriv = "0x1111111111111111111111111111111111111111111111111111111111111111";
    const uint64_t kNonce = 1700000000000ULL;

    json::object order;
    order["a"] = 0;
    order["b"] = true;
    order["p"] = "50000.0";
    order["s"] = "0.001";
    order["r"] = false;
    order["t"] = json::object{{"limit", json::object{{"tif", "Gtc"}}}};

    json::object action;
    action["type"] = "order";
    action["orders"] = json::array{std::move(order)};
    action["grouping"] = "na";

    HyperliquidSigner signer(kPriv, /*is_mainnet=*/false);
    auto tx = signer.sign_l1_action(action, kNonce);

    EXPECT_EQ(tx.r, "3a025d224dadc41491ea9c3217f14c33f4f82f51354644c6c95e3dcceb79a1a1");
    EXPECT_EQ(tx.s, "5d76cacab271d1323388dfd4a17eed118087eb5e41836940abf09d7d3884f1e9");
    EXPECT_EQ(tx.v, 28);
    EXPECT_EQ(tx.nonce, kNonce);
}

TEST(HyperliquidSignerTest, NonceIsMonotonic) {
    HyperliquidSigner signer("0x1111111111111111111111111111111111111111111111111111111111111111",
                             /*is_mainnet=*/false);
    uint64_t a = signer.next_nonce();
    uint64_t b = signer.next_nonce();
    uint64_t c = signer.next_nonce();
    EXPECT_LT(a, b);
    EXPECT_LT(b, c);
}

TEST(HyperliquidSignerTest, RejectsBadKey) {
    EXPECT_THROW(HyperliquidSigner("", false), std::runtime_error);
    EXPECT_THROW(HyperliquidSigner("deadbeef", false), std::runtime_error);
    EXPECT_THROW(
        HyperliquidSigner("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", false),
        std::runtime_error);
}

}  // namespace
