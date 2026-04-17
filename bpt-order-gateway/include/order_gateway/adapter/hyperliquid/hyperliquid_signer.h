#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include <boost/json/fwd.hpp>

namespace bpt::order_gateway::adapter {

// Signed Hyperliquid transaction — returned by HyperliquidSigner.
// r/s are 32-byte hex strings with NO "0x" prefix. v is 27 or 28 (the
// recovery id + 27, as Hyperliquid expects).
struct SignedTransaction {
    std::string r;
    std::string s;
    uint8_t v;
    uint64_t nonce;
};

// HyperliquidSigner holds the private key and signs L1 actions.
//
// Hyperliquid's L1 signing scheme (from the official Python SDK):
//   1. msgpack-encode the action JSON object (insertion-order, no sorting)
//   2. connectionId = keccak256(msgpack || nonce_BE_8 || vault_byte || expires_byte)
//      vault_byte   = 0x00 if no vault, else 0x01 || vault_address(20)
//      expires_byte = 0x00 if no expires, else 0x01 || expires_BE_8
//   3. Build EIP-712 typed data:
//        domain   = {name:"Exchange", version:"1", chainId:1337,
//                    verifyingContract:0x0000000000000000000000000000000000000000}
//        struct   = Agent(string source, bytes32 connectionId)
//        source   = "a" (mainnet) or "b" (testnet)
//   4. Sign keccak256("\x19\x01" || domainSep || structHash) via secp256k1
//      with a canonical-low-s signature, recover id → v ∈ {27,28}
//
// SECURITY:
//   - Key is passed in as a hex string (with or without 0x prefix).
//   - Key bytes live in a member array zeroed on destruction.
//   - No method exposes the key bytes and no intermediate values are logged.
//   - Final class, non-copyable, non-movable.
class HyperliquidSigner final {
public:
    // hex: 64-char secp256k1 private key (optionally 0x-prefixed).
    // is_mainnet: determines the "source" field in the Agent struct
    //             ("a" for mainnet, "b" for testnet).
    HyperliquidSigner(std::string_view hex, bool is_mainnet);
    ~HyperliquidSigner();

    HyperliquidSigner(const HyperliquidSigner&) = delete;
    HyperliquidSigner& operator=(const HyperliquidSigner&) = delete;
    HyperliquidSigner(HyperliquidSigner&&) = delete;
    HyperliquidSigner& operator=(HyperliquidSigner&&) = delete;

    // Sign an L1 action (e.g. {"type":"order",...} or {"type":"cancel",...}).
    // The caller owns nonce assignment — use next_nonce() for a
    // monotonic timestamp-based counter.
    [[nodiscard]] SignedTransaction sign_l1_action(const boost::json::value& action, uint64_t nonce) const;

    // Returns a monotonic, unique nonce suitable for use with Hyperliquid.
    // Implemented as max(now_ms, last+1).
    [[nodiscard]] uint64_t next_nonce() noexcept;

    bool is_mainnet() const noexcept { return is_mainnet_; }

private:
    std::array<uint8_t, 32> key_{};
    bool is_mainnet_{false};
    std::atomic<uint64_t> last_nonce_{0};
};

}  // namespace bpt::order_gateway::adapter
