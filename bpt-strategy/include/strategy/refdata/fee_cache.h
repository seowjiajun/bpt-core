#pragma once

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace bpt::strategy::refdata {

struct FeeScheduleEntry {
    int16_t maker_bps{0};
    int16_t taker_bps{0};
    uint64_t updated_ts{0};  // UTC nanosecond epoch when fetched from exchange
};

// Per-exchange default fee slot — padded to a full cache line to prevent
// false sharing between exchanges on the hot path.
struct alignas(64) ExchangeFeeSlot {
    std::atomic<int16_t> maker_bps{0};
    std::atomic<int16_t> taker_bps{0};
    std::atomic<uint64_t> updated_ts{0};
    // Remaining bytes of the 64-byte cache line are padding.
    char _pad[64 - 2 * sizeof(std::atomic<int16_t>) - sizeof(std::atomic<uint64_t>)]{};
};

// Thread-safe fee cache.
//
// Storage model:
//   - exchange_slots_[exchange_id]   : exchange-wide default fee (instrument_id == 0).
//     Read-path is a single cache-line load — safe for hot-path queries.
//   - per_instrument_                : per-instrument overrides (Binance per-symbol fees).
//     Protected by a shared_mutex; populated during Sindri snapshot, rarely updated.
//
// Staleness: any entry whose updated_ts is older than max_staleness_ns_ is treated as
// not-ready (get() returns nullopt).  max_staleness_ns_ is set from config at construction.
class FeeCache {
public:
    explicit FeeCache(uint64_t max_staleness_ns) : max_staleness_ns_(max_staleness_ns) {}

    // Update from a FeeSchedule Aeron message.
    // instrument_id == 0 → exchange-wide slot; otherwise per-instrument map.
    void update(bpt::messages::ExchangeId::Value exchange_id,
                uint64_t instrument_id,
                int16_t maker_bps,
                int16_t taker_bps,
                uint64_t updated_ts);

    // Drop the per-instrument entry for instrument_id, if any. Called
    // by RefdataClient on a REMOVE delta so the map doesn't accumulate
    // entries for delisted instruments across long-running sessions.
    // No-op on exchange-wide slots (those are keyed by ExchangeId, not
    // instrument_id, and there's one per exchange so they're bounded).
    void remove(uint64_t instrument_id);

    // Lookup fees for a specific instrument.
    // Falls back to the exchange-wide slot if no per-instrument entry exists.
    // Returns nullopt if no entry found or the entry is stale relative to now_ns.
    [[nodiscard]] std::optional<FeeScheduleEntry> get(bpt::messages::ExchangeId::Value exchange_id,
                                                      uint64_t instrument_id,
                                                      uint64_t now_ns) const;

private:
    // 8 slots — one per possible ExchangeId value (values are 0–7).
    ExchangeFeeSlot exchange_slots_[8];

    mutable std::shared_mutex mutex_;
    std::unordered_map<uint64_t, FeeScheduleEntry> per_instrument_;

    uint64_t max_staleness_ns_;
};

}  // namespace bpt::strategy::refdata
