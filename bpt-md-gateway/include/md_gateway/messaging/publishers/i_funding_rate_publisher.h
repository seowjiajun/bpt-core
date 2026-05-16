#pragma once

/// \file
/// \brief Outbound port: per-instrument funding-rate updates.
///
/// Funding rates ride a separate Aeron stream from the main MD feed
/// (`settings.aeron.funding_rate.stream_id` — typically 1005) so that
/// strategy can subscribe to them independently of the high-frequency
/// BBO/Trade fan-out on stream 2002. Adapters surface funding-rate
/// events via the `on_funding_rate` callback set by MdGatewayService; that
/// callback ends up calling publish() on the implementation of this
/// port.
///
/// Implementations: FundingRatePublisher (Aeron-backed) in prod. There
/// is no test fake yet — funding-rate paths are exercised end-to-end
/// via the venue decoder component tests.

#include <messages/ExchangeId.h>

#include <cstdint>
#include <functional>

namespace bpt::md_gateway::messaging {

/// \brief Domain struct passed across the port — venue-agnostic shape.
struct FundingRateUpdate {
    uint64_t instrument_id;                        ///< canonical refdata ID
    bpt::messages::ExchangeId::Value exchange_id;  ///< source venue tag for downstream filtering
    int32_t rate_bps;                              ///< signed; rate * 1e6 (e.g. 0.0001 → 100)
    uint64_t next_funding_ts_ns;                   ///< 0 if the venue does not provide a schedule
    uint64_t collected_ts_ns;                      ///< wall-clock recv time stamped by the adapter
};

/// \brief Per-event callback shape used inside the venue decoders.
///
/// The decoder calls this to surface a parsed funding-rate event;
/// MdGatewayService wires the callback to forward into the bus's
/// IFundingRatePublisher. Decoupled like this so the decoder doesn't
/// depend on the publisher type.
using FundingRateCallback = std::function<void(const FundingRateUpdate&)>;

/// \brief Contract for the funding-rate outbound port.
///
/// Called from each adapter's IO thread (one writer per adapter), but
/// multiple adapter threads may publish concurrently — implementations
/// must be thread-safe for publish().
class IFundingRatePublisher {
public:
    virtual ~IFundingRatePublisher() = default;

    /// \brief Encode and publish one funding-rate update on the funding stream.
    virtual void publish(const FundingRateUpdate& fr) = 0;
};

}  // namespace bpt::md_gateway::messaging
