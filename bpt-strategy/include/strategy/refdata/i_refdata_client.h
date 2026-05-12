#pragma once

/// @file
/// IRefdataClient — abstract interface for the strategy's refdata client.
///
/// Implementations:
///   AeronRefdataClient   — production path; consumes refdata snapshots,
///                          deltas, fees, funding, and status over Aeron.
///   InProcessRefdataClient — deterministic backtest path; loads the
///                          refdata snapshot from a JSON file synchronously.
///
/// Strategy code holds an IRefdataClient&; the bus factory injects whichever
/// implementation matches the run mode. Cache types (InstrumentCache,
/// FeeCache, FundingRateCache) are value/cache classes — kept here so both
/// implementations can populate the same cache shape.

#include "strategy/refdata/fee_cache.h"
#include "strategy/refdata/funding_rate_cache.h"
#include "strategy/refdata/instrument.h"
#include "strategy/refdata/instrument_cache.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>
#include <messages/RefDataErrorType.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bpt::strategy::refdata {

class IRefdataClient {
public:
    using OnSnapshotFn = std::function<void(const InstrumentCache&)>;
    using OnDeltaFn = std::function<void(const Instrument&, bpt::messages::DeltaUpdateType::Value)>;

    /// Canonical filter entry to pre-filter the snapshot server-side.
    /// An empty exchange means "any exchange".
    struct CanonicalFilter {
        std::string base;
        std::string quote;
        bpt::messages::InstrumentType::Value instrument_type;
        std::string exchange;
    };

    virtual ~IRefdataClient() = default;

    /// Send a subscription request with canonical filters so the server
    /// pre-filters the snapshot. An empty filters vector means subscribe-all
    /// (receive the full universe).
    virtual void subscribe(uint64_t correlation_id, std::vector<CanonicalFilter> filters = {}) = 0;

    /// Poll all streams. Returns total fragment count processed.
    /// Backtest impls may return 0 (no polling).
    virtual int poll(int fragment_limit = 10) = 0;

    /// Nanosecond timestamp of the last heartbeat received (0 if none yet).
    [[nodiscard]] virtual uint64_t last_heartbeat_ns() const = 0;

    /// Cache accessors. The caches are populated by the implementation;
    /// callers read through these refs.
    [[nodiscard]] virtual const InstrumentCache& cache() const = 0;
    [[nodiscard]] virtual const FeeCache& fee_cache() const = 0;
    [[nodiscard]] virtual const FundingRateCache& funding_rate_cache() const = 0;

    /// Fired once after the snapshot for our correlation ID is fully received.
    OnSnapshotFn on_snapshot_complete;

    /// Fired for every delta received after snapshot.
    OnDeltaFn on_delta;

    /// Fired when a delta sequence gap is detected. The cache has already
    /// been reset; the handler should trigger a resubscription
    /// (e.g. call subscribe() again).
    std::function<void()> on_gap_detected;

    /// Fired when the refdata service signals it has completed startup
    /// for all configured exchanges.
    /// exchanges_loaded: bitmask — bit0=BINANCE, bit1=OKX, bit2=HYPERLIQUID.
    std::function<
        void(uint8_t exchanges_loaded, uint16_t instrument_count, bool fee_schedules_loaded, bool funding_rates_loaded)>
        on_ready;

    /// Fired when the refdata service reports a runtime error Strategy must act on.
    std::function<void(bpt::messages::RefDataErrorType::Value error_type,
                       bpt::messages::ExchangeId::Value exchange_id,
                       uint64_t instrument_id)>
        on_error;
};

}  // namespace bpt::strategy::refdata
