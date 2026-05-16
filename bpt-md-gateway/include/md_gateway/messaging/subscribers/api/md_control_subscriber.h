#pragma once

/// \file
/// \brief Inbound port: SBE `MdSubscribeBatch` control fragments from strategy.
///
/// Strategy publishes subscribe / unsubscribe batches on the control
/// channel (`settings.aeron.md_control.stream_id`). MdGatewayService::run()
/// drains them every iteration of its main loop by calling poll();
/// each decoded batch is handed off to SubscriptionManager.
///
/// The port speaks the SBE wire type directly (`MdSubscribeBatch&`)
/// rather than a domain wrapper. This is an accepted compromise —
/// SubscriptionManager already takes the SBE type, so introducing a
/// domain struct here would just translate twice. Same shape refdata
/// uses on its inbound port.
///
/// Implementations: aeron::MdControlSubscriber in prod. A fake
/// implementation can drive subscribe-batches directly from a test for
/// seam testing without an Aeron MediaDriver.

#include <functional>

namespace bpt::messages {
class MdSubscribeBatch;
}

namespace bpt::md_gateway::messaging::api {

/// \brief Contract for the inbound control source.
///
/// Single-threaded contract: poll() is called from MdGatewayService's main
/// loop only. Implementations need not be thread-safe.
class MdControlSubscriber {
public:
    /// \brief Per-fragment handler invoked by poll() for each decoded batch.
    ///
    /// The reference is only valid for the duration of the call —
    /// implementations must not retain it past handler return (the
    /// underlying Aeron buffer slot may be reclaimed).
    using BatchHandler = std::function<void(bpt::messages::MdSubscribeBatch&)>;

    virtual ~MdControlSubscriber() = default;

    /// \brief Drain available control fragments, dispatching each to `handler`.
    ///
    /// Non-blocking: returns immediately if nothing is queued. Caller
    /// is expected to sleep / yield between polls when idle (the app
    /// loop sleeps 10us on a zero return).
    ///
    /// \return Number of fragments processed; 0 means idle.
    virtual int poll(BatchHandler handler) = 0;
};

}  // namespace bpt::md_gateway::messaging::api
