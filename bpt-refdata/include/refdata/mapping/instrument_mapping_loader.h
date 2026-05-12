#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::refdata::mapping {

// Exchange ID assignments (must match instrument_mapping.json forward key prefix).
// These are stable integers — do not renumber without updating the mapping file
// and all consumers.
constexpr uint8_t EXCHANGE_ID_BINANCE = 1;
constexpr uint8_t EXCHANGE_ID_OKX = 2;
constexpr uint8_t EXCHANGE_ID_HYPERLIQUID = 3;
constexpr uint8_t EXCHANGE_ID_DERIBIT = 4;

// inst_uid encoding: canonical_id * INST_UID_MULTIPLIER + exchange_id.
//
// Multiple exchanges share the same canonical_id (e.g. BTC PERP = 1001 on all
// three exchanges). To give each (canonical, exchange) pair a unique key in the
// InstrumentRegistry, we embed the exchange_id in the low bits.
//
// Examples:
//   Binance  BTC PERP (1001): inst_uid = 100101
//   OKX      BTC PERP (1001): inst_uid = 100102
//   HL       BTC PERP (1001): inst_uid = 100103
//   Binance  BTC SPOT (2001): inst_uid = 200101
constexpr uint64_t INST_UID_MULTIPLIER = 100;

inline uint64_t make_inst_uid(uint32_t canonical_id, uint8_t exchange_id) {
    return static_cast<uint64_t>(canonical_id) * INST_UID_MULTIPLIER + exchange_id;
}

inline uint32_t canonical_id_from_uid(uint64_t inst_uid) {
    return static_cast<uint32_t>(inst_uid / INST_UID_MULTIPLIER);
}

struct InstrumentInfo {
    std::string base;
    std::string quote;
    std::string type;  // "PERP", "SPOT", "FUTURE"
};

// One row in the universe view — emitted by instruments_for_venue() so
// callers can iterate the catalog without poking at private maps. Stable
// across reloads; the loader rebuilds entries from the JSON each load().
struct InstrumentEntry {
    uint32_t canonical_id;
    uint8_t exchange_id;
    std::string venue_symbol;  // exchange-native string (e.g. "BTC-USDT-SWAP", "BTC")
    InstrumentInfo info;
};

// Loads and caches the instrument mapping file (instrument_mapping.json).
//
// Refresh is driven externally — call load() again after a new file has been
// written (e.g. after a successful S3 fetch).  Thread-safe for concurrent reads.
//
// Forward map key format: "{exchange_id}_{exchange_symbol}"
//   Binance PERP BTCUSDT      → "1_BTCUSDT"
//   Binance SPOT BTCUSDT      → "1_BTCUSDT_SPOT"  (Binance reuses symbol across types)
//   OKX PERP BTC-USDT-SWAP    → "2_BTC-USDT-SWAP"
//   Hyperliquid PERP BTC      → "3_BTC"
//
// Canonical ID ranges: 1001–1999 PERP, 2001–2999 SPOT, 3001–3999 FUTURES.
class InstrumentMappingLoader {
public:
    static constexpr uint32_t UNKNOWN_INSTRUMENT = 0;

    InstrumentMappingLoader() = default;
    ~InstrumentMappingLoader() = default;

    // Load (or reload) mapping from file.
    // Throws std::runtime_error on missing file or parse error.
    // Thread-safe — in-flight readers are not interrupted during a reload.
    void load(const std::string& path);

    // Silent lookup — for bulk loads where a miss is expected (instrument not in
    // mapping). Returns nullopt on miss; no log emitted.
    [[nodiscard]] std::optional<uint32_t> try_resolve_canonical_id(uint8_t exchange_id,
                                                                   const std::string& exchange_symbol) const;

    // Targeted lookup — logs WARN on miss and returns UNKNOWN_INSTRUMENT (0).
    [[nodiscard]] uint32_t resolve_canonical_id(uint8_t exchange_id, const std::string& exchange_symbol) const;

    // canonical_id + exchange_id → exchange symbol. Returns empty string on miss.
    [[nodiscard]] std::string resolve_symbol(uint32_t canonical_id, uint8_t exchange_id) const;

    // canonical_id → InstrumentInfo. Returns nullopt on miss.
    [[nodiscard]] std::optional<InstrumentInfo> get_instrument_info(uint32_t canonical_id) const;

    [[nodiscard]] std::size_t instrument_count() const;

    // Snapshot view: every instrument in the mapping that has a listing
    // for the requested exchange_id. Returned by value (small per-process
    // catalog, hundreds-of-KB at most) so callers can iterate without
    // holding the read lock. bpt-tape uses this to build its universe
    // from the JSON without needing a running refdata service.
    [[nodiscard]] std::vector<InstrumentEntry> instruments_for_venue(uint8_t exchange_id) const;

private:
    struct ReverseEntry {
        InstrumentInfo info;
        std::unordered_map<uint8_t, std::string> exchanges;  // exchange_id → symbol
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, uint32_t> forward_;   // "{id}_{sym}" → canonical_id
    std::unordered_map<uint32_t, ReverseEntry> reverse_;  // canonical_id → entry
    std::size_t instrument_count_{0};
};

}  // namespace bpt::refdata::mapping
