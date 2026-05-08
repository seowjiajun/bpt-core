#pragma once

/// @file
/// InProcessRefdataClient — push-driven IRefdataClient for the
/// deterministic backtest harness. The harness loads the refdata
/// snapshot from a JSON file at startup (same instrument_mapping.json
/// the live refdata service consumes) and fires the snapshot-complete
/// callback synchronously. There are no live deltas in backtest mode —
/// instrument metadata is frozen for the duration of the replay.
///
/// Cache accessors return refs to in-memory caches the harness
/// populates. Funding-rate and fee-schedule caches are similarly
/// pre-loaded; future extension can wire fee-tier transitions or
/// funding-rate ticks into push_* methods if a strategy depends on
/// them.

#include "strategy/refdata/i_refdata_client.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace bpt::strategy::refdata {

class InProcessRefdataClient : public IRefdataClient {
public:
    /// `max_staleness_ns` propagates into FeeCache + FundingRateCache —
    /// matches the production AeronRefdataClient ctor's last argument.
    /// Default = 1 hour, ample for backtest replay (the strategy's
    /// stale-gate fires off the heartbeat clock, not the cache TTL).
    explicit InProcessRefdataClient(uint64_t max_staleness_ns = 3'600'000'000'000ULL)
        : fee_cache_(max_staleness_ns),
          funding_rate_cache_(max_staleness_ns) {}

    /// Strategy calls this on startup. The harness has already
    /// populated the cache before strategy is run, so subscribe is
    /// the moment we synthesise the on_snapshot_complete + on_ready
    /// signal — same lifecycle the Aeron variant produces.
    void subscribe(uint64_t correlation_id,
                   std::vector<CanonicalFilter> /*filters*/ = {}) override {
        last_correlation_id_ = correlation_id;
        if (on_snapshot_complete) on_snapshot_complete(cache_);
        if (on_ready) {
            on_ready(/*exchanges_loaded=*/exchanges_loaded_,
                     /*instrument_count=*/instrument_count_,
                     /*fee_schedules_loaded=*/fee_schedules_loaded_,
                     /*funding_rates_loaded=*/funding_rates_loaded_);
        }
    }

    int poll(int /*fragment_limit*/ = 10) override { return 0; }

    [[nodiscard]] uint64_t last_heartbeat_ns() const override { return last_heartbeat_ns_; }
    [[nodiscard]] const InstrumentCache& cache() const override { return cache_; }
    [[nodiscard]] const FeeCache& fee_cache() const override { return fee_cache_; }
    [[nodiscard]] const FundingRateCache& funding_rate_cache() const override { return funding_rate_cache_; }

    /// Harness-side population API. Caller pre-loads everything before
    /// strategy.start() runs subscribe(); after that the cache is
    /// frozen for the run.
    [[nodiscard]] InstrumentCache& mutable_cache() { return cache_; }
    [[nodiscard]] FeeCache& mutable_fee_cache() { return fee_cache_; }
    [[nodiscard]] FundingRateCache& mutable_funding_rate_cache() { return funding_rate_cache_; }

    void set_ready_state(uint8_t exchanges_loaded,
                         uint16_t instrument_count,
                         bool fee_schedules_loaded,
                         bool funding_rates_loaded) {
        exchanges_loaded_ = exchanges_loaded;
        instrument_count_ = instrument_count;
        fee_schedules_loaded_ = fee_schedules_loaded;
        funding_rates_loaded_ = funding_rates_loaded;
    }

    /// Harness pushes a heartbeat at the simulated timestamp so
    /// strategy's stale-gate logic uses replay time, not wallclock.
    void push_heartbeat(uint64_t ts_ns) { last_heartbeat_ns_ = ts_ns; }

private:
    uint64_t last_correlation_id_{0};
    uint64_t last_heartbeat_ns_{0};

    InstrumentCache cache_;
    FeeCache fee_cache_;
    FundingRateCache funding_rate_cache_;

    uint8_t exchanges_loaded_{0};
    uint16_t instrument_count_{0};
    bool fee_schedules_loaded_{false};
    bool funding_rates_loaded_{false};
};

}  // namespace bpt::strategy::refdata
