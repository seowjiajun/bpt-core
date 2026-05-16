#include "radar/messaging/subscribers/vol_surface_subscriber.h"

#include <messages/MessageHeader.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::radar::messaging {

using bpt::messages::MessageHeader;
using bpt::messages::VolSurface;

VolSurfaceSubscriber::VolSurfaceSubscriber(std::shared_ptr<aeron::Aeron> aeron,
                                           const std::string& channel,
                                           int stream_id) {
    sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, stream_id);
    assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buffer,
               aeron::util::index_t offset,
               aeron::util::index_t length,
               aeron::Header& header) { handle_fragment(buffer, offset, length, header); });

    bpt::common::log::info("[VolSurfaceSubscriber] ready on stream {}", stream_id);
}

int VolSurfaceSubscriber::poll(int fragment_limit) {
    if (!sub_)
        return 0;
    return sub_->poll(assembler_->handler(), fragment_limit);
}

void VolSurfaceSubscriber::handle_fragment(aeron::AtomicBuffer& buffer,
                                           aeron::util::index_t offset,
                                           aeron::util::index_t length,
                                           aeron::Header& /*header*/) {
    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

    if (hdr.templateId() != VolSurface::sbeTemplateId())
        return;

    VolSurface msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<uint64_t>(length));

    if (on_vol_surface)
        on_vol_surface(msg);
}

}  // namespace bpt::radar::messaging
