#include "order_gateway/messaging/publishers/aeron/heartbeat_publisher.h"

#include <bpt_common/util/tsc_clock.h>
#include <cstddef>

namespace bpt::order_gateway::messaging::aeron {

using bpt::common::util::WallClock;
using Policy = bpt::common::aeron::Publisher::Policy;

HeartbeatPublisher::HeartbeatPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id)
    // Heartbeats are strictly idempotent — next tick republishes. Drop
    // on no-subscriber; bounded retry on back-pressure.
    : publisher_(std::move(aeron), channel, stream_id, Policy::kBoundedRetry) {}

void HeartbeatPublisher::publish(uint8_t service_id, uint16_t orders_in_flight, uint8_t exchange_status) {
    alignas(8) std::byte scratch[SbeOrderGatewayHeartbeatCodec::kRecommendedScratchSize];
    OrderGatewayHeartbeatMsg m{service_id, WallClock::now_ns(), orders_in_flight, exchange_status};
    const auto bytes = codec_.encode(m, scratch);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(bytes.size()));
}

}  // namespace bpt::order_gateway::messaging::aeron
