#pragma once

/// \file
/// \brief Outbound port: OrderGatewayHeartbeat publish.
///
/// Publishes the per-tick liveness heartbeat carrying open-order count
/// and the per-venue connection bitmask. Strategy uses this to detect
/// a wedged or disconnected order-gateway and to know which venues are
/// currently routable.
///
/// Implementations: aeron::HeartbeatPublisher in prod.

#include <cstdint>

namespace bpt::order_gateway::messaging::api {

/// \brief Contract for the heartbeat outbound port.
///
/// Called from the main poll thread on a fixed cadence
/// (cfg.gateway.heartbeat_interval_ms). Single-threaded contract;
/// implementations need not be thread-safe.
class HeartbeatPublisher {
public:
    virtual ~HeartbeatPublisher() = default;

    /// \brief Publish one heartbeat frame.
    /// \param service_id        identifier for this order-gateway instance (typically 1)
    /// \param orders_in_flight  total open orders across all venues
    /// \param exchange_status   bitmask: bit0=BINANCE, bit1=OKX, bit2=HYPERLIQUID, bit3=DERIBIT;
    ///                          1 = connected
    virtual void publish(uint8_t service_id, uint16_t orders_in_flight, uint8_t exchange_status) = 0;
};

}  // namespace bpt::order_gateway::messaging::api
