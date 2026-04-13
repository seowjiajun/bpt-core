#include "heimdall/adapter/hyperliquid/hyperliquid_signer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <secp256k1.h>
#include <secp256k1_recovery.h>

namespace heimdall::adapter {

namespace json = boost::json;

// ──────────────────────────────────────────────────────────────────────────────
// Keccak-256 (raw keccak, not SHA3) — inline so we don't depend on a
// specific OpenSSL build. Verified against the Python reference.
// ──────────────────────────────────────────────────────────────────────────────

namespace {

const uint64_t KECCAK_RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};
const int KECCAK_ROT[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
const int KECCAK_PI[24]  = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

inline uint64_t rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

void keccak_f1600(uint64_t st[25]) {
    for (int round = 0; round < 24; ++round) {
        uint64_t bc[5];
        for (int i = 0; i < 5; ++i)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        uint64_t t[5];
        for (int i = 0; i < 5; ++i)
            t[i] = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
        for (int i = 0; i < 25; ++i) st[i] ^= t[i % 5];

        uint64_t last = st[1];
        for (int i = 0; i < 24; ++i) {
            int j = KECCAK_PI[i];
            uint64_t tmp = st[j];
            st[j] = rotl64(last, KECCAK_ROT[i]);
            last = tmp;
        }
        for (int i = 0; i < 25; i += 5) {
            uint64_t tmp[5];
            for (int j = 0; j < 5; ++j) tmp[j] = st[i + j];
            for (int j = 0; j < 5; ++j)
                st[i + j] = tmp[j] ^ (~tmp[(j + 1) % 5] & tmp[(j + 2) % 5]);
        }
        st[0] ^= KECCAK_RC[round];
    }
}

void keccak256(const uint8_t* in, std::size_t in_len, uint8_t out[32]) {
    uint64_t state[25] = {};
    constexpr std::size_t RATE = 136;

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
    uint8_t block[RATE] = {};
    std::size_t rem = in_len - pos;
    std::memcpy(block, in + pos, rem);
    block[rem] = 0x01;  // keccak padding (SHA3 would be 0x06)
    block[RATE - 1] ^= 0x80;
    for (std::size_t i = 0; i < RATE / 8; ++i) {
        uint64_t tmp;
        std::memcpy(&tmp, block + i * 8, 8);
        state[i] ^= tmp;
    }
    keccak_f1600(state);
    std::memcpy(out, state, 32);
}

std::array<uint8_t, 32> keccak256_arr(const uint8_t* in, std::size_t len) {
    std::array<uint8_t, 32> out{};
    keccak256(in, len, out.data());
    return out;
}

// ──────────────────────────────────────────────────────────────────────────────
// MessagePack encoder
//
// Hyperliquid requires msgpack-encoding the action JSON before hashing.
// The bytes must match Python's `msgpack.packb(action)` exactly — Python's
// default uses the current (2.0+) spec:
//   - strings as str family (0xa0..0xbf / 0xd9 / 0xda / 0xdb)
//   - integers via most compact form (pos fixint, uint8/16/32/64, neg fixint / int8..int64)
//   - float64 for floats (0xcb)  [HL actions use strings for prices, not floats]
//   - bool 0xc2 / 0xc3, nil 0xc0
//   - fixmap/map16/map32, fixarray/array16/array32
//
// Object keys come from boost::json::object iteration, which preserves
// insertion order — matching Python dict ordering. Do NOT sort keys.
// ──────────────────────────────────────────────────────────────────────────────

void mp_append_u8(std::vector<uint8_t>& out, uint8_t b) { out.push_back(b); }
void mp_append_be(std::vector<uint8_t>& out, uint64_t v, int nbytes) {
    for (int i = nbytes - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

void mp_pack_uint(std::vector<uint8_t>& out, uint64_t v) {
    if (v <= 0x7F) {
        mp_append_u8(out, static_cast<uint8_t>(v));
    } else if (v <= 0xFF) {
        mp_append_u8(out, 0xCC);
        mp_append_u8(out, static_cast<uint8_t>(v));
    } else if (v <= 0xFFFF) {
        mp_append_u8(out, 0xCD);
        mp_append_be(out, v, 2);
    } else if (v <= 0xFFFFFFFFULL) {
        mp_append_u8(out, 0xCE);
        mp_append_be(out, v, 4);
    } else {
        mp_append_u8(out, 0xCF);
        mp_append_be(out, v, 8);
    }
}

void mp_pack_int(std::vector<uint8_t>& out, int64_t v) {
    if (v >= 0) {
        mp_pack_uint(out, static_cast<uint64_t>(v));
        return;
    }
    if (v >= -32) {
        mp_append_u8(out, static_cast<uint8_t>(v));  // neg fixint
    } else if (v >= -128) {
        mp_append_u8(out, 0xD0);
        mp_append_u8(out, static_cast<uint8_t>(v));
    } else if (v >= -32768) {
        mp_append_u8(out, 0xD1);
        mp_append_be(out, static_cast<uint64_t>(static_cast<int16_t>(v)) & 0xFFFFULL, 2);
    } else if (v >= -2147483648LL) {
        mp_append_u8(out, 0xD2);
        mp_append_be(out, static_cast<uint64_t>(static_cast<int32_t>(v)) & 0xFFFFFFFFULL, 4);
    } else {
        mp_append_u8(out, 0xD3);
        mp_append_be(out, static_cast<uint64_t>(v), 8);
    }
}

void mp_pack_str(std::vector<uint8_t>& out, std::string_view s) {
    const std::size_t n = s.size();
    if (n <= 31) {
        mp_append_u8(out, static_cast<uint8_t>(0xA0 | n));
    } else if (n <= 0xFF) {
        mp_append_u8(out, 0xD9);
        mp_append_u8(out, static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        mp_append_u8(out, 0xDA);
        mp_append_be(out, n, 2);
    } else {
        mp_append_u8(out, 0xDB);
        mp_append_be(out, n, 4);
    }
    out.insert(out.end(), s.begin(), s.end());
}

void mp_pack_array_header(std::vector<uint8_t>& out, std::size_t n) {
    if (n <= 15) {
        mp_append_u8(out, static_cast<uint8_t>(0x90 | n));
    } else if (n <= 0xFFFF) {
        mp_append_u8(out, 0xDC);
        mp_append_be(out, n, 2);
    } else {
        mp_append_u8(out, 0xDD);
        mp_append_be(out, n, 4);
    }
}

void mp_pack_map_header(std::vector<uint8_t>& out, std::size_t n) {
    if (n <= 15) {
        mp_append_u8(out, static_cast<uint8_t>(0x80 | n));
    } else if (n <= 0xFFFF) {
        mp_append_u8(out, 0xDE);
        mp_append_be(out, n, 2);
    } else {
        mp_append_u8(out, 0xDF);
        mp_append_be(out, n, 4);
    }
}

void mp_pack_float64(std::vector<uint8_t>& out, double v) {
    mp_append_u8(out, 0xCB);
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    mp_append_be(out, bits, 8);
}

void mp_pack_value(std::vector<uint8_t>& out, const json::value& v) {
    switch (v.kind()) {
        case json::kind::null:
            mp_append_u8(out, 0xC0);
            return;
        case json::kind::bool_:
            mp_append_u8(out, v.get_bool() ? 0xC3 : 0xC2);
            return;
        case json::kind::int64:
            mp_pack_int(out, v.get_int64());
            return;
        case json::kind::uint64:
            mp_pack_uint(out, v.get_uint64());
            return;
        case json::kind::double_:
            mp_pack_float64(out, v.get_double());
            return;
        case json::kind::string: {
            const auto& s = v.get_string();
            mp_pack_str(out, std::string_view(s.data(), s.size()));
            return;
        }
        case json::kind::array: {
            const auto& a = v.get_array();
            mp_pack_array_header(out, a.size());
            for (const auto& e : a) mp_pack_value(out, e);
            return;
        }
        case json::kind::object: {
            const auto& o = v.get_object();
            mp_pack_map_header(out, o.size());
            for (const auto& kv : o) {
                mp_pack_str(out, std::string_view(kv.key().data(), kv.key().size()));
                mp_pack_value(out, kv.value());
            }
            return;
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// EIP-712 primitives
//
// For strings and bytes, EIP-712 encodes the keccak256 of the raw bytes
// in place. For bytes32, the bytes are used directly. Uint256 values are
// right-aligned in a 32-byte big-endian word. Addresses are 20 bytes
// zero-padded on the left to 32 bytes.
// ──────────────────────────────────────────────────────────────────────────────

// Precomputed keccak256 of the domain type and Agent type strings — saves
// a hash per signing. Verified once against the Python reference on boot
// via the reference test.

// Domain separator: keccak256(typeHash || nameHash || versionHash || chainId || verifyingContract)
// where:
//   typeHash        = keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
//   nameHash        = keccak256("Exchange")
//   versionHash     = keccak256("1")
//   chainId         = 1337 as uint256 BE
//   verifyingContract = 0x000...000 (20 zero bytes, padded to 32)
std::array<uint8_t, 32> build_hl_domain_separator() {
    static const char kDomainType[] =
        "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    static const char kName[] = "Exchange";
    static const char kVersion[] = "1";

    auto th = keccak256_arr(reinterpret_cast<const uint8_t*>(kDomainType), sizeof(kDomainType) - 1);
    auto nh = keccak256_arr(reinterpret_cast<const uint8_t*>(kName), sizeof(kName) - 1);
    auto vh = keccak256_arr(reinterpret_cast<const uint8_t*>(kVersion), sizeof(kVersion) - 1);

    std::vector<uint8_t> enc;
    enc.reserve(32 * 5);
    enc.insert(enc.end(), th.begin(), th.end());
    enc.insert(enc.end(), nh.begin(), nh.end());
    enc.insert(enc.end(), vh.begin(), vh.end());

    // chainId = 1337 = 0x539 as 32-byte BE
    uint8_t chain_id[32] = {};
    chain_id[30] = 0x05;
    chain_id[31] = 0x39;
    enc.insert(enc.end(), chain_id, chain_id + 32);

    // verifyingContract = 20 zero bytes → 32 zero bytes
    uint8_t verifying[32] = {};
    enc.insert(enc.end(), verifying, verifying + 32);

    return keccak256_arr(enc.data(), enc.size());
}

// Agent type hash: keccak256("Agent(string source,bytes32 connectionId)")
std::array<uint8_t, 32> agent_type_hash() {
    static const char kAgentType[] = "Agent(string source,bytes32 connectionId)";
    return keccak256_arr(reinterpret_cast<const uint8_t*>(kAgentType), sizeof(kAgentType) - 1);
}

// Struct hash for Agent{source, connectionId}
std::array<uint8_t, 32> agent_struct_hash(const char* source,
                                          const std::array<uint8_t, 32>& connection_id) {
    auto type_hash = agent_type_hash();
    std::array<uint8_t, 32> source_hash =
        keccak256_arr(reinterpret_cast<const uint8_t*>(source), std::strlen(source));

    std::vector<uint8_t> enc;
    enc.reserve(96);
    enc.insert(enc.end(), type_hash.begin(), type_hash.end());
    enc.insert(enc.end(), source_hash.begin(), source_hash.end());
    enc.insert(enc.end(), connection_id.begin(), connection_id.end());

    return keccak256_arr(enc.data(), enc.size());
}

// ──────────────────────────────────────────────────────────────────────────────
// Hex helpers
// ──────────────────────────────────────────────────────────────────────────────

std::string bytes_to_hex(const uint8_t* data, std::size_t len) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    return oss.str();
}

uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    throw std::runtime_error("Invalid hex character");
}

// ──────────────────────────────────────────────────────────────────────────────
// secp256k1 singleton context — verified + signed flags; created once.
// ──────────────────────────────────────────────────────────────────────────────

secp256k1_context* secp_ctx() {
    static secp256k1_context* ctx = [] {
        return secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    }();
    return ctx;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────────
// HyperliquidSigner
// ──────────────────────────────────────────────────────────────────────────────

HyperliquidSigner::HyperliquidSigner(std::string_view hex, bool is_mainnet) : is_mainnet_(is_mainnet) {
    if (hex.empty())
        throw std::runtime_error("HyperliquidSigner: private key is empty");
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex.remove_prefix(2);
    if (hex.size() != 64)
        throw std::runtime_error("HyperliquidSigner: private key must be 64 hex characters");
    for (std::size_t i = 0; i < 32; ++i)
        key_[i] = static_cast<uint8_t>((hex_nibble(hex[i * 2]) << 4) | hex_nibble(hex[i * 2 + 1]));

    // Sanity: verify the key is in range for secp256k1 before anyone tries to sign.
    if (!secp256k1_ec_seckey_verify(secp_ctx(), key_.data()))
        throw std::runtime_error("HyperliquidSigner: invalid secp256k1 private key");
}

HyperliquidSigner::~HyperliquidSigner() {
    volatile uint8_t* p = key_.data();
    for (std::size_t i = 0; i < key_.size(); ++i) p[i] = 0;
}

uint64_t HyperliquidSigner::next_nonce() noexcept {
    // Hyperliquid nonces are millisecond timestamps, strictly increasing.
    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    uint64_t prev = last_nonce_.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        next = std::max(now_ms, prev + 1);
    } while (!last_nonce_.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    return next;
}

SignedTransaction HyperliquidSigner::sign_l1_action(const json::value& action, uint64_t nonce) const {
    // Step 1: msgpack-encode the action (deterministic, matches Python msgpack.packb).
    std::vector<uint8_t> mp;
    mp.reserve(256);
    mp_pack_value(mp, action);

    // Step 2: append nonce BE-8 and vault/expires markers (both absent = 0x00).
    mp.push_back(static_cast<uint8_t>((nonce >> 56) & 0xFF));
    mp.push_back(static_cast<uint8_t>((nonce >> 48) & 0xFF));
    mp.push_back(static_cast<uint8_t>((nonce >> 40) & 0xFF));
    mp.push_back(static_cast<uint8_t>((nonce >> 32) & 0xFF));
    mp.push_back(static_cast<uint8_t>((nonce >> 24) & 0xFF));
    mp.push_back(static_cast<uint8_t>((nonce >> 16) & 0xFF));
    mp.push_back(static_cast<uint8_t>((nonce >> 8) & 0xFF));
    mp.push_back(static_cast<uint8_t>(nonce & 0xFF));
    mp.push_back(0x00);  // no vault
    // Note: no expires trailer — the Python SDK omits the byte entirely when
    // expires_after is None (it does NOT append 0x00).

    auto connection_id = keccak256_arr(mp.data(), mp.size());

    // Step 3: build Agent struct hash.
    const char* source = is_mainnet_ ? "a" : "b";
    auto struct_hash = agent_struct_hash(source, connection_id);

    // Step 4: final EIP-712 digest = keccak256("\x19\x01" || domainSep || structHash)
    static const std::array<uint8_t, 32> domain_sep = build_hl_domain_separator();
    std::array<uint8_t, 66> final_input{};
    final_input[0] = 0x19;
    final_input[1] = 0x01;
    std::memcpy(final_input.data() + 2, domain_sep.data(), 32);
    std::memcpy(final_input.data() + 34, struct_hash.data(), 32);
    auto final_hash = keccak256_arr(final_input.data(), final_input.size());

    // Step 5: sign with recoverable ECDSA.
    secp256k1_ecdsa_recoverable_signature rsig;
    if (!secp256k1_ecdsa_sign_recoverable(secp_ctx(), &rsig, final_hash.data(), key_.data(), nullptr, nullptr))
        throw std::runtime_error("HyperliquidSigner: secp256k1_ecdsa_sign_recoverable failed");

    uint8_t sig_compact[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(secp_ctx(), sig_compact, &recid, &rsig);

    SignedTransaction tx;
    tx.r = bytes_to_hex(sig_compact, 32);
    tx.s = bytes_to_hex(sig_compact + 32, 32);
    tx.v = static_cast<uint8_t>(recid + 27);
    tx.nonce = nonce;
    return tx;
}

}  // namespace heimdall::adapter
