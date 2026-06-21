#pragma once

/// \file
/// \brief IExchangeRefDataAdapter — the per-venue refdata interface used by bpt-refdata.

#include "refdata/model/fee_schedule.h"
#include "refdata/model/instrument.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>

#include <functional>
#include <string>

namespace bpt::refdata::adapter {

/// \brief Abstract base for per-exchange reference data adapters.
///
/// **Lifecycle**:
///   1. Construct (no network activity).
///   2. `fetchSnapshot()`   — blocking REST snapshot; populates registry; sets isReady().
///   3. `subscribeDeltas()` — no-op (funding rates moved to MdGateway; no WS needed).
///   4. `stop()`            — no-op.
///
/// Hourly REST poll for instrument listing changes is driven externally
/// (main loop calls `fetchInstrumentListing()` via a scheduled timer).
class IExchangeRefDataAdapter {
public:
    virtual ~IExchangeRefDataAdapter() = default;

    /// \brief Blocking REST snapshot fetch — MUST complete before subscribeDeltas().
    virtual void fetchSnapshot() = 0;

    /// \brief No-op: funding rates have moved to MdGateway; no WS subscriptions needed.
    virtual void subscribeDeltas() = 0;

    /// \brief Re-fetch the instrument listing from REST (called hourly by main loop).
    virtual void fetchInstrumentListing() = 0;

    /// \brief Graceful shutdown.
    virtual void stop() = 0;

    virtual bool isReady() const = 0;
    virtual const char* exchange_name() const = 0;
    virtual bpt::messages::ExchangeId::Value exchange_id() const = 0;

    /// \brief Fired for each instrument ADD/MODIFY/REMOVE.
    std::function<void(const model::Instrument&, bpt::messages::DeltaUpdateType::Value, uint64_t collected_ts_ns)>
        on_instrument_delta;

    /// \brief Fired on startup (from snapshot) and whenever fees change.
    std::function<void(const model::FeeScheduleState&)> on_fee_schedule;
};

}  // namespace bpt::refdata::adapter
