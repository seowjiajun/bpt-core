#include "order_gateway/messaging/heartbeat_publisher.h"

#include <messages/MessageHeader.h>
#include <messages/OrderGatewayHeartbeat.h>

#include <chrono>

namespace bpt::order_gateway::messaging {

using Policy = bpt::common::aeron::Publisher::Policy;

HeartbeatPublisher::HeartbeatPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                       const std::string& channel, int stream_id)
    // Heartbeats are strictly idempotent — next tick republishes. Drop
    // on no-subscriber; bounded retry on back-pressure.
    : publisher_(std::move(aeron), channel, stream_id, Policy::kBoundedRetry) {}

void HeartbeatPublisher::publish(uint8_t service_id, uint16_t orders_in_flight, uint8_t exchange_status) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + OrderGatewayHeartbeat::sbeBlockLength();
    char buf[kBufSize]{};

    OrderGatewayHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .serviceId(service_id)
        .timestampNs(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count()))
        .ordersInFlight(orders_in_flight)
        .exchangeStatus(exchange_status);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<::aeron::util::index_t>(kBufSize));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(kBufSize));
}

}  // namespace bpt::order_gateway::messaging
