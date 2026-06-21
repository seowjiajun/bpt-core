#include "md_gateway/messaging/publishers/aeron/ack_publisher.h"

#include <bpt_common/util/tsc_clock.h>
#include <cstddef>

namespace bpt::md_gateway::messaging::aeron {

using bpt::common::util::WallClock;
using Policy = bpt::common::aeron::Publisher::Policy;

AckPublisher::AckPublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream)
    // Subscription acks + heartbeats are idempotent/republished. Bounded
    // retry on back-pressure, drop on no subscriber.
    : publisher_(std::move(aeron), stream.channel, stream.stream_id, Policy::kBoundedRetry) {}

void AckPublisher::publish_ack(uint64_t correlation_id,
                               uint64_t instrument_id,
                               const char* exchange,
                               bpt::messages::AckStatus::Value status) {
    alignas(8) std::byte scratch[SbeMdSubscriptionAckCodec::kRecommendedScratchSize];
    MdSubscriptionAckMsg m{correlation_id, WallClock::now_ns(), instrument_id, status, std::string(exchange)};
    const auto bytes = ack_codec_.encode(m, scratch);
    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(bytes.size()));
}

void AckPublisher::publish_subscription_heartbeat(uint64_t instrument_id) {
    alignas(8) std::byte scratch[SbeMdSubscriptionHeartbeatCodec::kRecommendedScratchSize];
    MdSubscriptionHeartbeatMsg m{WallClock::now_ns(), instrument_id, seq_.fetch_add(1, std::memory_order_relaxed) + 1};
    const auto bytes = sub_hb_codec_.encode(m, scratch);
    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(bytes.size()));
}

void AckPublisher::publish_service_heartbeat() {
    alignas(8) std::byte scratch[SbeMdServiceHeartbeatCodec::kRecommendedScratchSize];
    MdServiceHeartbeatMsg m{WallClock::now_ns(), seq_.fetch_add(1, std::memory_order_relaxed) + 1};
    const auto bytes = svc_hb_codec_.encode(m, scratch);
    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(bytes.size()));
}

}  // namespace bpt::md_gateway::messaging::aeron
