#pragma once

/// \file
/// \brief Pure interface every md-gateway venue adapter implements.

#include "md_gateway/messaging/funding_rate_publisher.h"

#include <bpt_common/util/latency_histogram.h>
#include <bpt_common/util/topology.h>
#include <cstdint>
#include <functional>
#include <string>

namespace bpt::md_gateway::adapter {

/// \brief Contract for exchange-specific market-data adapters.
///
/// Each adapter:
///   - Manages a WebSocket connection to one venue.
///   - Subscribes / unsubscribes instruments on demand.
///   - Normalises venue-specific JSON into MdMarketData / MdTrade SBE
///     messages and publishes them via MdPublisher.
///   - Runs on its own thread (start() launches; stop() joins).
class IAdapter {
public:
    virtual ~IAdapter() = default;

    /// \brief Subscribe to BBO + trade feed for the given instrument.
    /// \param instrument_id  canonical refdata ID, stamped onto every downstream message
    /// \param symbol         exchange-native symbol (e.g. "BTCUSDT" for Binance)
    /// \param depth          0 = BBO only; 5 = top-5 ladder; venue-specific cap
    virtual void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) = 0;

    /// \brief Unsubscribe from an instrument. No-op if not subscribed.
    virtual void unsubscribe(uint64_t instrument_id) = 0;

    /// \brief Launch the adapter's IO thread and connect to the venue WebSocket.
    virtual void start() = 0;

    /// \brief Signal the adapter to stop, wait for the IO thread to join.
    virtual void stop() = 0;

    /// \brief Bind the central CPU-affinity topology.
    ///
    /// Must be called before start() — the IO thread reads its role
    /// assignment the moment it launches. A topology that doesn't carry
    /// the adapter's role falls through as unpinned with an INFO log
    /// (dev-laptop default).
    virtual void set_topology(const bpt::common::util::Topology& topology) = 0;

    /// \brief Human-readable venue name for logging (e.g. "BINANCE").
    [[nodiscard]] virtual const char* exchange_name() const = 0;

    /// \brief Decode-latency histogram for the metrics reporter to sample.
    [[nodiscard]] virtual bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept = 0;

    /// \brief MD messages that passed validation and were published to Aeron.
    [[nodiscard]] virtual uint64_t md_published_count() const noexcept = 0;

    /// \brief MD messages dropped by the validator (price-deviation, etc.).
    [[nodiscard]] virtual uint64_t validation_drop_count() const noexcept = 0;

    /// \brief True if the ValidationDropBreaker has latched on this adapter.
    ///
    /// Exposed for the periodic Prometheus gauge sampler in MdGatewayService.
    [[nodiscard]] virtual bool validation_drop_breaker_tripped() const noexcept { return false; }

    /// Called from the adapter's IO thread when a funding-rate update arrives. Set before start().
    messaging::FundingRateCallback on_funding_rate;

    /// \name Connection-lifecycle hooks
    ///
    /// Called from the IO thread. on_connect fires after a successful
    /// WebSocket handshake + subscribe; on_disconnect fires on unexpected
    /// connection loss, just before the reconnect-backoff delay.
    /// \{
    std::function<void()> on_connect;
    std::function<void()> on_disconnect;
    /// \}
};

}  // namespace bpt::md_gateway::adapter
