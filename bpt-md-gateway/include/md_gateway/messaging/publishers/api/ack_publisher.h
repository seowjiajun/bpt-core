#pragma once

/// \file
/// \brief Outbound port: ACKs and heartbeats published toward strategy.
///
/// Three message types share this port because they are all small,
/// low-frequency notifications strategy needs to know about and they
/// all ride the same Aeron stream (`settings.aeron.md_ack_hb.stream_id`).
/// Implementations: aeron::AckPublisher in prod; FakeAckPublisher in
/// component tests.

#include <messages/AckStatus.h>

#include <cstdint>

namespace bpt::md_gateway::messaging::api {

/// \brief Contract for the ACK / heartbeat outbound port.
///
/// All methods are called from MdGatewayService::run() on the main poll
/// thread. No thread-safety contract is required from implementations.
class AckPublisher {
public:
    virtual ~AckPublisher() = default;

    /// \brief One ACK per (subscribe, instrument) pair after a control batch is applied.
    ///
    /// \param correlation_id  echoes the strategy's MdSubscribeBatch.correlationId for matching
    /// \param instrument_id   canonical refdata ID
    /// \param exchange        venue name string (e.g. "OKX") — written into the ACK as venue
    /// \param status          AckStatus enum (ACCEPTED / REJECTED / etc.)
    virtual void publish_ack(uint64_t correlation_id,
                             uint64_t instrument_id,
                             const char* exchange,
                             bpt::messages::AckStatus::Value status) = 0;

    /// \brief Per-instrument heartbeat — strategy uses these to detect a wedged subscription.
    ///
    /// Cadence governed by `settings.subscription_heartbeat_interval_ms`.
    virtual void publish_subscription_heartbeat(uint64_t instrument_id) = 0;

    /// \brief Process-level liveness ping. One per `service_heartbeat_interval_ms`.
    virtual void publish_service_heartbeat() = 0;
};

}  // namespace bpt::md_gateway::messaging::api
