#include "strategy/refdata/fee_cache.h"

#include <mutex>
#include <shared_mutex>

namespace bpt::strategy::refdata {

void FeeCache::update(bpt::messages::ExchangeId::Value exchange_id,
                      uint64_t instrument_id,
                      int16_t maker_bps,
                      int16_t taker_bps,
                      uint64_t updated_ts) {
    if (instrument_id == 0) {
        // Exchange-wide default: update the per-exchange aligned slot lock-free.
        auto idx = static_cast<uint8_t>(exchange_id);
        if (idx >= 8)
            return;
        exchange_slots_[idx].maker_bps.store(maker_bps, std::memory_order_release);
        exchange_slots_[idx].taker_bps.store(taker_bps, std::memory_order_release);
        exchange_slots_[idx].updated_ts.store(updated_ts, std::memory_order_release);
    } else {
        std::unique_lock lock(mutex_);
        per_instrument_[instrument_id] = {maker_bps, taker_bps, updated_ts};
    }
}

void FeeCache::remove(uint64_t instrument_id) {
    if (instrument_id == 0) return;  // exchange-wide slot isn't in the map
    std::unique_lock lock(mutex_);
    per_instrument_.erase(instrument_id);
}

std::optional<FeeScheduleEntry> FeeCache::get(bpt::messages::ExchangeId::Value exchange_id,
                                              uint64_t instrument_id,
                                              uint64_t now_ns) const {
    // First try per-instrument map.
    if (instrument_id != 0) {
        std::shared_lock lock(mutex_);
        auto it = per_instrument_.find(instrument_id);
        if (it != per_instrument_.end()) {
            const auto& e = it->second;
            if (now_ns - e.updated_ts <= max_staleness_ns_)
                return e;
            return std::nullopt;  // stale
        }
        // Fall through to exchange-wide default.
    }

    auto idx = static_cast<uint8_t>(exchange_id);
    if (idx >= 8)
        return std::nullopt;

    FeeScheduleEntry e;
    e.maker_bps = exchange_slots_[idx].maker_bps.load(std::memory_order_acquire);
    e.taker_bps = exchange_slots_[idx].taker_bps.load(std::memory_order_acquire);
    e.updated_ts = exchange_slots_[idx].updated_ts.load(std::memory_order_acquire);

    if (e.updated_ts == 0)
        return std::nullopt;  // never populated
    if (now_ns - e.updated_ts > max_staleness_ns_)
        return std::nullopt;  // stale

    return e;
}

}  // namespace bpt::strategy::refdata
