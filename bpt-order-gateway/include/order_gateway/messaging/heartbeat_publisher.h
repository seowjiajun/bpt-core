#pragma once

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <string>
#include <bpt_common/aeron/publisher.h>

namespace bpt::order_gateway::messaging {

class HeartbeatPublisher {
public:
    HeartbeatPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    // Publish a OrderGatewayHeartbeat.
    // service_id: identifier for this order-gateway instance (typically 1).
    // orders_in_flight: number of open orders across all venues.
    // exchange_status: bitmask — bit0=BINANCE, bit1=OKX, bit2=HYPERLIQUID;
    // 1=connected.
    void publish(uint8_t service_id, uint16_t orders_in_flight, uint8_t exchange_status);

private:
    bpt::common::aeron::Publisher publisher_;
};

}  // namespace bpt::order_gateway::messaging
