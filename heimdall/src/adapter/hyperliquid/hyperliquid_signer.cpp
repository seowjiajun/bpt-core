#include "heimdall/adapter/hyperliquid/hyperliquid_signer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace heimdall::adapter {

// ──────────────────────────────────────────────────────────────────────────────
// Keccak-256 implementation (raw keccak, not SHA3-256)
// OpenSSL 3.x exposes "KECCAK-256" only in some builds; we implement it inline
// to avoid a build-time dependency on a specific OpenSSL version.
// ──────────────────────────────────────────────────────────────────────────────

namespace keccak_impl {

static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL, 0x000000000000808bULL,
    0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL, 0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

static const int ROT[24] = {
    1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
};

static const int PI[24] = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
};

static inline uint64_t rotl64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

static void keccak_f1600(uint64_t st[25]) {
    for (int round = 0; round < 24; ++round) {
        // Theta
        uint64_t bc[5];
        for (int i = 0; i < 5; ++i)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];

        uint64_t t[5];
        for (int i = 0; i < 5; ++i)
            t[i] = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);

        for (int i = 0; i < 25; ++i)
            st[i] ^= t[i % 5];

        // Rho + Pi
        uint64_t last = st[1];
        for (int i = 0; i < 24; ++i) {
            int j = PI[i];
            uint64_t tmp = st[j];
            st[j] = rotl64(last, ROT[i]);
            last = tmp;
        }

        // Chi
        for (int i = 0; i < 25; i += 5) {
            uint64_t tmp[5];
            for (int j = 0; j < 5; ++j)
                tmp[j] = st[i + j];
            for (int j = 0; j < 5; ++j)
                st[i + j] = tmp[j] ^ (~tmp[(j + 1) % 5] & tmp[(j + 2) % 5]);
        }

        // Iota
        st[0] ^= RC[round];
    }
}

// Compute keccak-256 (not SHA3) of input data.
static void keccak256(const uint8_t* in, std::size_t in_len, uint8_t out[32]) {
    uint64_t state[25] = {};

    // Rate for keccak-256: 1088 bits = 136 bytes
    static constexpr std::size_t RATE = 136;
    static constexpr uint8_t DOMAIN = 0x01;  // keccak padding (not SHA3 = 0x06)

    std::size_t pos = 0;
    while (in_len - pos >= RATE) {
        for (std::size_t i = 0; i < RATE / 8; ++i) {
            uint64_t tmp;
            std::memcpy(&tmp, in + pos + i * 8, 8);
            state[i] ^= tmp;
        }
        keccak_f1600(state);
        pos += RATE;
    }

    // Final block
    uint8_t block[RATE] = {};
    std::size_t rem = in_len - pos;
    std::memcpy(block, in + pos, rem);
    block[rem] = DOMAIN;
    block[RATE - 1] ^= 0x80;

    for (std::size_t i = 0; i < RATE / 8; ++i) {
        uint64_t tmp;
        std::memcpy(&tmp, block + i * 8, 8);
        state[i] ^= tmp;
    }
    keccak_f1600(state);

    std::memcpy(out, state, 32);
}

}  // namespace keccak_impl

// ──────────────────────────────────────────────────────────────────────────────
// Hex helpers
// ──────────────────────────────────────────────────────────────────────────────

static std::string bytes_to_hex(const uint8_t* data, std::size_t len) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    return oss.str();
}

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<uint8_t>(c - 'A' + 10);
    throw std::runtime_error("Invalid hex character");
}

// ──────────────────────────────────────────────────────────────────────────────
// HyperliquidSigner implementation
// ──────────────────────────────────────────────────────────────────────────────

HyperliquidSigner::HyperliquidSigner(std::string_view hex) {
    if (hex.empty())
        throw std::runtime_error("HyperliquidSigner: private key is empty");

    // Accept both "0x"-prefixed and raw hex — "0x" is standard Ethereum
    // tooling convention (wallets, MetaMask exports, etc. include it).
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex.remove_prefix(2);

    if (hex.size() != 64)
        throw std::runtime_error("HyperliquidSigner: private key must be exactly 64 hex characters (32 bytes)");

    for (std::size_t i = 0; i < 32; ++i)
        key_[i] = static_cast<uint8_t>((hex_nibble(hex[i * 2]) << 4) | hex_nibble(hex[i * 2 + 1]));
}

HyperliquidSigner::~HyperliquidSigner() {
    // Zero out the key — use volatile to prevent compiler optimization
    volatile uint8_t* p = key_.data();
    for (std::size_t i = 0; i < key_.size(); ++i)
        p[i] = 0;
}

uint64_t HyperliquidSigner::next_nonce() noexcept {
    return ++nonce_;
}

std::array<uint8_t, 32> HyperliquidSigner::keccak256(const uint8_t* data, std::size_t len) const {
    std::array<uint8_t, 32> out{};
    keccak_impl::keccak256(data, len, out.data());
    return out;
}

SignedTransaction HyperliquidSigner::ecdsa_sign(const std::array<uint8_t, 32>& hash) const {
    // Build secp256k1 private key using the OpenSSL 3.0 EVP_PKEY API.
    BIGNUM* priv_bn = BN_bin2bn(key_.data(), 32, nullptr);
    if (!priv_bn)
        throw std::runtime_error("BN_bin2bn failed");

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if (!bld) {
        BN_free(priv_bn);
        throw std::runtime_error("OSSL_PARAM_BLD_new failed");
    }
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "secp256k1", 0);
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv_bn);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    OSSL_PARAM_BLD_free(bld);
    BN_free(priv_bn);
    if (!params)
        throw std::runtime_error("OSSL_PARAM_BLD_to_param failed");

    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!kctx) {
        OSSL_PARAM_free(params);
        throw std::runtime_error("EVP_PKEY_CTX_new_from_name failed");
    }
    if (EVP_PKEY_fromdata_init(kctx) <= 0) {
        EVP_PKEY_CTX_free(kctx);
        OSSL_PARAM_free(params);
        throw std::runtime_error("EVP_PKEY_fromdata_init failed");
    }
    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_fromdata(kctx, &pkey, EVP_PKEY_KEYPAIR, params) <= 0) {
        EVP_PKEY_CTX_free(kctx);
        OSSL_PARAM_free(params);
        throw std::runtime_error("EVP_PKEY_fromdata failed");
    }
    EVP_PKEY_CTX_free(kctx);
    OSSL_PARAM_free(params);

    // Sign the pre-hashed digest — produces a DER-encoded ECDSA signature.
    EVP_PKEY_CTX* sctx = EVP_PKEY_CTX_new(pkey, nullptr);
    if (!sctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_PKEY_CTX_new failed");
    }
    if (EVP_PKEY_sign_init(sctx) <= 0) {
        EVP_PKEY_CTX_free(sctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_PKEY_sign_init failed");
    }

    std::size_t sig_len = 0;
    EVP_PKEY_sign(sctx, nullptr, &sig_len, hash.data(), hash.size());
    std::vector<uint8_t> sig_der(sig_len);
    if (EVP_PKEY_sign(sctx, sig_der.data(), &sig_len, hash.data(), hash.size()) <= 0) {
        EVP_PKEY_CTX_free(sctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_PKEY_sign failed");
    }
    EVP_PKEY_CTX_free(sctx);
    EVP_PKEY_free(pkey);

    // Decode DER to extract r and s.
    const uint8_t* p = sig_der.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig_len));
    if (!sig)
        throw std::runtime_error("d2i_ECDSA_SIG failed");

    const BIGNUM* r_bn;
    const BIGNUM* s_bn;
    ECDSA_SIG_get0(sig, &r_bn, &s_bn);

    SignedTransaction tx;
    tx.nonce = nonce_;

    uint8_t r_bytes[32] = {};
    uint8_t s_bytes[32] = {};
    BN_bn2bin(r_bn, r_bytes + (32 - BN_num_bytes(r_bn)));
    BN_bn2bin(s_bn, s_bytes + (32 - BN_num_bytes(s_bn)));

    tx.r = bytes_to_hex(r_bytes, 32);
    tx.s = bytes_to_hex(s_bytes, 32);
    tx.v = 27;  // Hyperliquid expects v in {27, 28}

    ECDSA_SIG_free(sig);
    return tx;
}

// EIP-712 typed data hash for Hyperliquid order action.
//
// Hyperliquid domain:
//   {"name":"Exchange","version":"1","chainId":1337}
//
// Order action type (simplified, matching HL public API):
//   OrderAction {
//     type: "order",
//     orders: OrderSpec[],
//     grouping: "na"
//   }
//
// This is a best-effort implementation of HL's EIP-712 structure.
// The exact schema is based on Hyperliquid's public documentation.
SignedTransaction HyperliquidSigner::sign_order(const OrderSignParams& params) {
    // Build the action string for signing (Hyperliquid uses a specific hash
    // format) HL signs: keccak256(abi.encode(action, nonce, vault_address)) For
    // simplicity, we build the canonical hash of the action

    uint64_t n = next_nonce();

    // Build a deterministic byte representation of the order for signing.
    // Hyperliquid's actual signing scheme:
    // hash = keccak256(encode(TypedData{domain, OrderAction}))
    //
    // Domain separator for HL mainnet (chain 1337):
    static const char kDomainStr[] = "EIP712Domain(string name,string version,uint256 chainId)";
    auto ds_type_hash = keccak256(reinterpret_cast<const uint8_t*>(kDomainStr), sizeof(kDomainStr) - 1);

    static const char kName[] = "Exchange";
    static const char kVersion[] = "1";
    auto name_hash = keccak256(reinterpret_cast<const uint8_t*>(kName), sizeof(kName) - 1);
    auto ver_hash = keccak256(reinterpret_cast<const uint8_t*>(kVersion), sizeof(kVersion) - 1);

    // chainId 1337 as 32-byte big-endian
    uint8_t chain_id[32] = {};
    chain_id[30] = 0x05;  // 1337 = 0x539
    chain_id[31] = 0x39;

    // domain separator = keccak256(abi.encode(typeHash, nameHash, versionHash,
    // chainId))
    std::vector<uint8_t> domain_enc;
    domain_enc.insert(domain_enc.end(), ds_type_hash.begin(), ds_type_hash.end());
    domain_enc.insert(domain_enc.end(), name_hash.begin(), name_hash.end());
    domain_enc.insert(domain_enc.end(), ver_hash.begin(), ver_hash.end());
    domain_enc.insert(domain_enc.end(), chain_id, chain_id + 32);
    auto domain_separator = keccak256(domain_enc.data(), domain_enc.size());

    // Order struct hash (simplified — coin + is_buy + price + size + reduce_only
    // + cloid + nonce)
    static const char kOrderTypeStr[] =
        "Order(string coin,bool isBuy,uint64 limitPx,uint64 sz,bool "
        "reduceOnly,uint64 cloid,uint64 nonce)";
    auto order_type_hash = keccak256(reinterpret_cast<const uint8_t*>(kOrderTypeStr), sizeof(kOrderTypeStr) - 1);

    auto coin_hash = keccak256(reinterpret_cast<const uint8_t*>(params.coin.data()), params.coin.size());

    // Encode order fields as 32-byte ABI values
    auto encode_bool = [](bool b) -> std::array<uint8_t, 32> {
        std::array<uint8_t, 32> r{};
        r[31] = b ? 1 : 0;
        return r;
    };
    auto encode_u64 = [](uint64_t v) -> std::array<uint8_t, 32> {
        std::array<uint8_t, 32> r{};
        for (int i = 7; i >= 0; --i) {
            r[31 - (7 - i)] = static_cast<uint8_t>(v & 0xFF);
            v >>= 8;
        }
        return r;
    };

    uint64_t price_scaled = static_cast<uint64_t>(params.price * 1e8);
    uint64_t size_scaled = static_cast<uint64_t>(params.size * 1e8);

    std::vector<uint8_t> order_enc;
    order_enc.insert(order_enc.end(), order_type_hash.begin(), order_type_hash.end());
    order_enc.insert(order_enc.end(), coin_hash.begin(), coin_hash.end());
    auto b_isbuy = encode_bool(params.is_buy);
    order_enc.insert(order_enc.end(), b_isbuy.begin(), b_isbuy.end());
    auto u_price = encode_u64(price_scaled);
    order_enc.insert(order_enc.end(), u_price.begin(), u_price.end());
    auto u_size = encode_u64(size_scaled);
    order_enc.insert(order_enc.end(), u_size.begin(), u_size.end());
    auto b_ro = encode_bool(params.reduce_only);
    order_enc.insert(order_enc.end(), b_ro.begin(), b_ro.end());
    auto u_cloid = encode_u64(params.cloid);
    order_enc.insert(order_enc.end(), u_cloid.begin(), u_cloid.end());
    auto u_nonce = encode_u64(n);
    order_enc.insert(order_enc.end(), u_nonce.begin(), u_nonce.end());
    auto struct_hash = keccak256(order_enc.data(), order_enc.size());

    // Final EIP-712 hash: keccak256("\x19\x01" + domainSeparator + structHash)
    std::vector<uint8_t> final_input;
    final_input.push_back(0x19);
    final_input.push_back(0x01);
    final_input.insert(final_input.end(), domain_separator.begin(), domain_separator.end());
    final_input.insert(final_input.end(), struct_hash.begin(), struct_hash.end());
    auto final_hash = keccak256(final_input.data(), final_input.size());

    return ecdsa_sign(final_hash);
}

SignedTransaction HyperliquidSigner::sign_cancel(const std::string& coin, uint64_t oid) {
    uint64_t n = next_nonce();

    static const char kDomainStr[] = "EIP712Domain(string name,string version,uint256 chainId)";
    auto ds_type_hash = keccak256(reinterpret_cast<const uint8_t*>(kDomainStr), sizeof(kDomainStr) - 1);

    static const char kName[] = "Exchange";
    static const char kVersion[] = "1";
    auto name_hash = keccak256(reinterpret_cast<const uint8_t*>(kName), sizeof(kName) - 1);
    auto ver_hash = keccak256(reinterpret_cast<const uint8_t*>(kVersion), sizeof(kVersion) - 1);
    uint8_t chain_id[32] = {};
    chain_id[30] = 0x05;
    chain_id[31] = 0x39;

    std::vector<uint8_t> domain_enc;
    domain_enc.insert(domain_enc.end(), ds_type_hash.begin(), ds_type_hash.end());
    domain_enc.insert(domain_enc.end(), name_hash.begin(), name_hash.end());
    domain_enc.insert(domain_enc.end(), ver_hash.begin(), ver_hash.end());
    domain_enc.insert(domain_enc.end(), chain_id, chain_id + 32);
    auto domain_separator = keccak256(domain_enc.data(), domain_enc.size());

    static const char kCancelTypeStr[] = "Cancel(string coin,uint64 oid,uint64 nonce)";
    auto cancel_type_hash = keccak256(reinterpret_cast<const uint8_t*>(kCancelTypeStr), sizeof(kCancelTypeStr) - 1);
    auto coin_hash = keccak256(reinterpret_cast<const uint8_t*>(coin.data()), coin.size());

    auto encode_u64 = [](uint64_t v) -> std::array<uint8_t, 32> {
        std::array<uint8_t, 32> r{};
        for (int i = 7; i >= 0; --i) {
            r[31 - (7 - i)] = static_cast<uint8_t>(v & 0xFF);
            v >>= 8;
        }
        return r;
    };

    std::vector<uint8_t> cancel_enc;
    cancel_enc.insert(cancel_enc.end(), cancel_type_hash.begin(), cancel_type_hash.end());
    cancel_enc.insert(cancel_enc.end(), coin_hash.begin(), coin_hash.end());
    auto u_oid = encode_u64(oid);
    cancel_enc.insert(cancel_enc.end(), u_oid.begin(), u_oid.end());
    auto u_nonce = encode_u64(n);
    cancel_enc.insert(cancel_enc.end(), u_nonce.begin(), u_nonce.end());
    auto struct_hash = keccak256(cancel_enc.data(), cancel_enc.size());

    std::vector<uint8_t> final_input;
    final_input.push_back(0x19);
    final_input.push_back(0x01);
    final_input.insert(final_input.end(), domain_separator.begin(), domain_separator.end());
    final_input.insert(final_input.end(), struct_hash.begin(), struct_hash.end());
    auto final_hash = keccak256(final_input.data(), final_input.size());

    return ecdsa_sign(final_hash);
}

}  // namespace heimdall::adapter
