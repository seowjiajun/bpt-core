#include "pricer/messaging/status_publisher.h"

#include <messages/MessageHeader.h>
#include <messages/PricerHeartbeat.h>
#include <messages/PricerReady.h>

#include <cstring>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::pricer::messaging {

StatusPublisher::StatusPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                 const std::string& channel,
                                 int32_t stream_id) {
    pub_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
    bpt::common::log::info("[StatusPublisher] Publication ready on {} stream {}", channel, stream_id);
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

    bpt::common::log::info("Published PricerReady: exchanges=0x{:02x} underlyings={} points={}",
                   exchanges_loaded,
                   underlying_count,
                   point_count);
}

}  // namespace bpt::pricer::messaging
