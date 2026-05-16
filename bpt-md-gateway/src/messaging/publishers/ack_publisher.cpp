#include "md_gateway/messaging/publishers/ack_publisher.h"

#include <messages/MessageHeader.h>

#include <bpt_common/util/tsc_clock.h>
#include <cstring>

namespace bpt::md_gateway::messaging {

using bpt::common::util::WallClock;
using bpt::messages::MdServiceHeartbeat;
using bpt::messages::MdSubscriptionAck;
using bpt::messages::MdSubscriptionHeartbeat;
using bpt::messages::MessageHeader;

using Policy = bpt::common::aeron::Publisher::Policy;

AckPublisher::AckPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id)
    // Subscription acks + heartbeats are idempotent/republished. Bounded
    // retry on back-pressure, drop on no subscriber.
    : publisher_(std::move(aeron), channel, stream_id, Policy::kBoundedRetry) {}

void AckPublisher::publish_ack(uint64_t correlation_id,
                               uint64_t instrument_id,
                               const char* exchange,
                               bpt::messages::AckStatus::Value status) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + MdSubscriptionAck::sbeBlockLength();
    char buf[kBufSize]{};

    MdSubscriptionAck msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .correlationId(correlation_id)
        .timestampNs(WallClock::now_ns())
        .instrumentId(instrument_id)
        .ackStatus(status);

    char* ex_field = msg.exchange();
    std::size_t ex_len = std::min(std::strlen(exchange), std::size_t{8});
    std::memcpy(ex_field, exchange, ex_len);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<::aeron::util::index_t>(kBufSize));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(kBufSize));
}

void AckPublisher::publish_subscription_heartbeat(uint64_t instrument_id) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + MdSubscriptionHeartbeat::sbeBlockLength();
    char buf[kBufSize]{};

    MdSubscriptionHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .timestampNs(WallClock::now_ns())
        .instrumentId(instrument_id)
        .seqNum(seq_.fetch_add(1, std::memory_order_relaxed) + 1);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<::aeron::util::index_t>(kBufSize));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(kBufSize));
}

void AckPublisher::publish_service_heartbeat() {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + MdServiceHeartbeat::sbeBlockLength();
    char buf[kBufSize]{};

    MdServiceHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .timestampNs(WallClock::now_ns())
        .seqNum(seq_.fetch_add(1, std::memory_order_relaxed) + 1);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<::aeron::util::index_t>(kBufSize));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(kBufSize));
}

}  // namespace bpt::md_gateway::messaging
