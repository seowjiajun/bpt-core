#pragma once

#include "order_gateway/messaging/codecs/sbe_order_gateway_heartbeat_codec.h"
#include "order_gateway/messaging/publishers/api/heartbeat_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::order_gateway::messaging::aeron {

/// \brief Aeron-backed concrete for api::HeartbeatPublisher.
///
/// Publishes OrderGatewayHeartbeat on its own Aeron stream. Driven
/// from the main poll thread on a fixed cadence (see
/// OrderGatewayService::run heartbeat path).
class HeartbeatPublisher final : public api::HeartbeatPublisher {
public:
    HeartbeatPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    /// \copydoc api::HeartbeatPublisher::publish
    void publish(uint8_t service_id, uint16_t orders_in_flight, uint8_t exchange_status) override;

private:
    bpt::common::aeron::Publisher    publisher_;
    SbeOrderGatewayHeartbeatCodec    codec_;
};

}  // namespace bpt::order_gateway::messaging::aeron
