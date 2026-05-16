#pragma once

/// \file
/// \brief Outbound port: slow-cadence per-instrument stats from the venue ticker channel.
///
/// Bundles the per-instrument numeric fields that venues publish on their
/// "ticker" channel — open interest, 24h volume, mark price, index price,
/// last trade price — into one snapshot message. Rides a separate Aeron
/// stream from the BBO firehose (`settings.aeron.instrument_stats.stream_id`
/// — typically 2004) because these fields update every few seconds at most;
/// republishing them on every BBO tick would force every strategy to decode
/// fields it does not care about.
///
/// Optional fields use NaN as the absent-value sentinel. The decoder
/// initialises every double to NaN and only overwrites those the venue
/// provided for this instrument type — spot tickers have no openInterest,
/// non-perp instruments have no markPrice/indexPrice, etc.
///
/// Adapters surface stats events via the `on_instrument_stats` callback
/// set by MdGatewayService; that callback ends up calling publish() on the
/// implementation of this port. Mirror of the funding-rate port pattern.

#include <messages/ExchangeId.h>

#include <cstdint>
#include <functional>
#include <limits>

namespace bpt::md_gateway::messaging {

/// \brief Domain struct passed across the port — venue-agnostic shape.
///
/// Lives in the parent `messaging` namespace (not `api`) so it can be
/// referenced from decoder templates and codec utilities without an
/// extra `api::` qualifier — it is a value type shared by both sides
/// of the port.
struct InstrumentStatsUpdate {
    uint64_t instrument_id;                        ///< canonical refdata ID
    bpt::messages::ExchangeId::Value exchange_id;  ///< source venue tag for downstream filtering

    double open_interest{std::numeric_limits<double>::quiet_NaN()};  ///< total OI in venue-native units
    double volume_24h{std::numeric_limits<double>::quiet_NaN()};     ///< trailing-24h notional volume
    double mark_price{std::numeric_limits<double>::quiet_NaN()};     ///< venue mark / settlement price
    double index_price{std::numeric_limits<double>::quiet_NaN()};    ///< underlying index used for mark
    double last_price{std::numeric_limits<double>::quiet_NaN()};     ///< most recent trade print

    uint64_t collected_ts_ns;  ///< wall-clock recv time stamped by the adapter
};

/// \brief Per-event callback shape used inside the venue decoders.
///
/// The decoder calls this to surface a parsed stats event; MdGatewayService
/// wires the callback to forward into the bus's api::InstrumentStatsPublisher.
/// Decoupled so the decoder doesn't depend on the publisher type.
using InstrumentStatsCallback = std::function<void(const InstrumentStatsUpdate&)>;

namespace api {

/// \brief Contract for the instrument-stats outbound port.
///
/// Called from each adapter's IO thread (one writer per adapter), but
/// multiple adapter threads may publish concurrently — implementations
/// must be thread-safe for publish().
class InstrumentStatsPublisher {
public:
    virtual ~InstrumentStatsPublisher() = default;

    /// \brief Encode and publish one stats update on the instrument-stats stream.
    virtual void publish(const InstrumentStatsUpdate& stats) = 0;
};

}  // namespace api

}  // namespace bpt::md_gateway::messaging
