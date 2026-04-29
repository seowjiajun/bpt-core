#pragma once

#include "order_gateway/messaging/i_heartbeat_publisher.h"

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <string>
#include <bpt_common/aeron/publisher.h>

namespace bpt::order_gateway::messaging {

/// \brief Aeron-backed concrete for IHeartbeatPublisher.
///
/// Publishes OrderGatewayHeartbeat on its own Aeron stream. Driven
/// from the main poll thread on a fixed cadence (see
/// OrderGatewayApp::run heartbeat path).
class HeartbeatPublisher final : public IHeartbeatPublisher {
public:
    HeartbeatPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                       const std::string& channel,
                       int stream_id);

    /// \copydoc IHeartbeatPublisher::publish
    void publish(uint8_t service_id,
                 uint16_t orders_in_flight,
                 uint8_t exchange_status) override;

private:
    bpt::common::aeron::Publisher publisher_;
};

}  // namespace bpt::order_gateway::messaging
