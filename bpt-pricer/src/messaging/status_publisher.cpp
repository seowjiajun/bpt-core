#include "pricer/messaging/status_publisher.h"

#include <messages/MessageHeader.h>
#include <messages/PricerHeartbeat.h>
#include <messages/PricerReady.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <yggdrasil/logging.h>

namespace bpt::pricer::messaging {

StatusPublisher::StatusPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                 const std::string& channel,
                                 int32_t stream_id,
                                 int pub_timeout_ms,
                                 int pub_poll_interval_ms) {
    const int64_t reg_id = aeron->addPublication(channel, stream_id);
    const int max_retries = pub_timeout_ms / std::max(pub_poll_interval_ms, 1);
    const auto poll_interval = std::chrono::milliseconds(pub_poll_interval_ms);

    for (int i = 0; i < max_retries; ++i) {
        pub_ = aeron->findPublication(reg_id);
        if (pub_)
            break;
        std::this_thread::sleep_for(poll_interval);
    }

    if (!pub_) {
        ygg::log::error("[StatusPublisher] Failed to find publication on {} stream {}", channel, stream_id);
    } else {
        ygg::log::info("[StatusPublisher] Publication ready on {} stream {}", channel, stream_id);
    }
}

void StatusPublisher::publish_heartbeat(uint64_t timestamp_ns, uint64_t seq_num) {
    if (!pub_)
        return;

    alignas(8) char buf[128];
    std::memset(buf, 0, sizeof(buf));

    using namespace bpt::messages;

    MessageHeader hdr;
    PricerHeartbeat msg;

    hdr.wrap(buf, 0, PricerHeartbeat::sbeSchemaVersion(), sizeof(buf))
        .blockLength(PricerHeartbeat::sbeBlockLength())
        .templateId(PricerHeartbeat::sbeTemplateId())
        .schemaId(PricerHeartbeat::sbeSchemaId())
        .version(PricerHeartbeat::sbeSchemaVersion());

    msg.wrapForEncode(buf, hdr.encodedLength(), sizeof(buf));
    msg.timestampNs(timestamp_ns);
    msg.seqNum(seq_num);

    const auto total = MessageHeader::encodedLength() + msg.encodedLength();
    aeron::concurrent::AtomicBuffer buffer(reinterpret_cast<uint8_t*>(buf), total);
    pub_->offer(buffer, 0, static_cast<int32_t>(total));
}

void StatusPublisher::publish_ready(uint64_t timestamp_ns,
                                    uint8_t exchanges_loaded,
                                    uint16_t underlying_count,
                                    uint32_t point_count) {
    if (!pub_)
        return;

    alignas(8) char buf[128];
    std::memset(buf, 0, sizeof(buf));

    using namespace bpt::messages;

    MessageHeader hdr;
    PricerReady msg;

    hdr.wrap(buf, 0, PricerReady::sbeSchemaVersion(), sizeof(buf))
        .blockLength(PricerReady::sbeBlockLength())
        .templateId(PricerReady::sbeTemplateId())
        .schemaId(PricerReady::sbeSchemaId())
        .version(PricerReady::sbeSchemaVersion());

    msg.wrapForEncode(buf, hdr.encodedLength(), sizeof(buf));
    msg.timestampNs(timestamp_ns);
    msg.exchangesLoaded(exchanges_loaded);
    msg.underlyingCount(underlying_count);
    msg.pointCount(point_count);

    const auto total = MessageHeader::encodedLength() + msg.encodedLength();
    aeron::concurrent::AtomicBuffer buffer(reinterpret_cast<uint8_t*>(buf), total);
    pub_->offer(buffer, 0, static_cast<int32_t>(total));

    ygg::log::info("[Pricer] Published PricerReady: exchanges=0x{:02x} underlyings={} points={}",
                   exchanges_loaded,
                   underlying_count,
                   point_count);
}

}  // namespace bpt::pricer::messaging
