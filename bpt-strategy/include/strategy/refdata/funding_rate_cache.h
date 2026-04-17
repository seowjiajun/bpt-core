#pragma once

#include <messages/ExchangeId.h>

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace bpt::strategy::refdata {

struct FundingRateEntry {
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id;
    int32_t rate_bps{0};          // signed; 1 bps = 0.01%; negative = rebate
    uint64_t next_funding_ts{0};  // UTC nanosecond epoch of next settlement (0 if unknown)
    uint64_t collected_ts{0};     // UTC nanosecond epoch when fetched from exchange
};

// Thread-safe funding rate cache.
//
// Keyed by instrument_id (canonical, exchange-independent UID).
// All updates and lookups are protected by a shared_mutex.
//
// Staleness: entries whose collected_ts is older than max_staleness_ns_ relative to
// now_ns are treated as stale and get() returns nullopt.  The staleness window should
// be set to several times the worst-case exchange update interval (e.g. 10+ minutes
// for exchanges that update every 8 hours).
class FundingRateCache {
public:
    explicit FundingRateCache(uint64_t max_staleness_ns) : max_staleness_ns_(max_staleness_ns) {}

    // Insert or replace the entry for the given instrument.
    void update(bpt::messages::ExchangeId::Value exchange_id,
                uint64_t instrument_id,
                int32_t rate_bps,
                uint64_t next_funding_ts,
                uint64_t collected_ts);

    // Lookup the current funding rate for an instrument.
    // Returns nullopt if no entry exists or the entry is stale relative to now_ns.
    [[nodiscard]] std::optional<FundingRateEntry> get(uint64_t instrument_id, uint64_t now_ns) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<uint64_t, FundingRateEntry> rates_;

    uint64_t max_staleness_ns_;
};

}  // namespace bpt::strategy::refdata
