#pragma once

#include "md_gateway/messaging/funding_rate_publisher.h"

#include <cstdint>
#include <functional>
#include <string>
#include <yggdrasil/util/latency_histogram.h>

namespace bpt::md_gateway::adapter {

// Pure interface for exchange-specific market-data adapters.
//
// Each adapter:
//  - Manages a WebSocket connection to one exchange.
//  - Subscribes/unsubscribes instruments on demand.
//  - Normalises exchange-specific JSON into MdMarketData / MdTrade SBE messages
//    and publishes them via MdPublisher.
//  - Runs on its own thread (start() launches it, stop() joins it).
class IAdapter {
public:
    virtual ~IAdapter() = default;

    // Subscribe to BBO + trade feed for the given instrument.
    // instrumentId: canonical Refdata ID used as the key in all downstream messages.
    // symbol: exchange-native symbol string (e.g. "BTCUSDT" for Binance).
    // depth: 0 = BBO only, 5 = top-5 order book levels, etc.
    virtual void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) = 0;

    // Unsubscribe from an instrument.  No-op if not subscribed.
    virtual void unsubscribe(uint64_t instrument_id) = 0;

    // Launch the adapter's I/O thread and connect to the exchange WebSocket.
    virtual void start() = 0;

    // Signal the adapter thread to stop, wait for it to join.
    virtual void stop() = 0;

    // Human-readable exchange name for logging (e.g. "BINANCE").
    [[nodiscard]] virtual const char* exchange_name() const = 0;

    // Returns a reference to the parser's decode latency histogram so the
    // metrics reporter can snapshot it periodically.
    [[nodiscard]] virtual ygg::util::LatencyHistogram& decode_latency_hist() noexcept = 0;

    // Monotonically increasing count of MD messages that passed validation and
    // were forwarded to the Aeron publisher.
    [[nodiscard]] virtual uint64_t md_published_count() const noexcept = 0;

    // Monotonically increasing count of MD messages dropped by the validator.
    [[nodiscard]] virtual uint64_t validation_drop_count() const noexcept = 0;

    // Set before start(). Called from the adapter's IO thread when a funding rate update arrives.
    messaging::FundingRateCallback on_funding_rate;

    // Optional callbacks for connection lifecycle events — called from the IO thread.
    // on_connect fires after a successful WebSocket handshake + subscribe.
    // on_disconnect fires on unexpected connection loss, just before the reconnect delay.
    std::function<void()> on_connect;
    std::function<void()> on_disconnect;
};

}  // namespace bpt::md_gateway::adapter
