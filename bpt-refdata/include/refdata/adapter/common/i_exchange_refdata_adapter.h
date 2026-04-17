#pragma once

#include "refdata/refdata/funding_rate.h"
#include "refdata/refdata/instrument.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>

#include <functional>
#include <string>

namespace bpt::refdata::adapter {

// Abstract base for per-exchange reference data adapters.
//
// Lifecycle:
//   1. Construct (no network activity)
//   2. fetchSnapshot()     — blocking REST snapshot; populates registry; sets isReady()
//   3. subscribeDeltas()   — no-op (funding rates moved to Huginn; no WS needed)
//   4. stop()              — no-op
//
// Hourly REST poll for instrument listing changes is driven externally
// (main loop calls fetchInstrumentListing() via a scheduled timer).
class IExchangeRefDataAdapter {
public:
    virtual ~IExchangeRefDataAdapter() = default;

    // Blocking REST snapshot fetch — MUST complete before subscribeDeltas().
    virtual void fetchSnapshot() = 0;

    // No-op: funding rates have moved to Huginn; no WS subscriptions needed.
    virtual void subscribeDeltas() = 0;

    // Re-fetch the instrument listing from REST (called hourly by main loop).
    virtual void fetchInstrumentListing() = 0;

    // Graceful shutdown.
    virtual void stop() = 0;

    virtual bool isReady() const = 0;
    virtual const char* exchange_name() const = 0;
    virtual bpt::messages::ExchangeId::Value exchange_id() const = 0;

    // Fired for each instrument ADD/MODIFY/REMOVE.
    std::function<void(const refdata::Instrument&, bpt::messages::DeltaUpdateType::Value, uint64_t collected_ts_ns)>
        on_instrument_delta;

    // Fired on startup (from snapshot) and whenever fees change.
    std::function<void(const refdata::FeeScheduleState&)> on_fee_schedule;
};

}  // namespace bpt::refdata::adapter
