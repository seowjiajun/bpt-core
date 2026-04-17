#include "strategy/refdata/funding_rate_cache.h"

#include <mutex>
#include <shared_mutex>

namespace bpt::strategy::refdata {

void FundingRateCache::update(bpt::messages::ExchangeId::Value exchange_id,
                              uint64_t instrument_id,
                              int32_t rate_bps,
                              uint64_t next_funding_ts,
                              uint64_t collected_ts) {
    std::unique_lock lock(mutex_);
    rates_[instrument_id] = {exchange_id, instrument_id, rate_bps, next_funding_ts, collected_ts};
}

std::optional<FundingRateEntry> FundingRateCache::get(uint64_t instrument_id, uint64_t now_ns) const {
    std::shared_lock lock(mutex_);
    auto it = rates_.find(instrument_id);
    if (it == rates_.end())
        return std::nullopt;

    const auto& e = it->second;
    if (now_ns - e.collected_ts > max_staleness_ns_)
        return std::nullopt;  // stale

    return e;
}

}  // namespace bpt::strategy::refdata
