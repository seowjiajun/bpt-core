#include "strategy/vol/vol_surface_client.h"

#include <messages/MessageHeader.h>
#include <messages/PricerHeartbeat.h>
#include <messages/PricerReady.h>
#include <messages/VolSurface.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::strategy::vol {

VolSurfaceClient::VolSurfaceClient(std::shared_ptr<aeron::Aeron> aeron,
                                   const std::string& channel,
                                   int vol_surface_stream,
                                   int pricer_status_stream) {
    surface_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, vol_surface_stream);
    status_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, pricer_status_stream);

    // FragmentAssembler for surface stream — VolSurface messages can be large
    surface_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buffer,
               aeron::util::index_t offset,
               aeron::util::index_t length,
               aeron::Header& header) { handle_surface_fragment(buffer, offset, length, header); });

    bpt::common::log::info("[VolSurfaceClient] Subscriptions ready: surface={} status={}",
                   vol_surface_stream,
                   pricer_status_stream);
}

int VolSurfaceClient::poll(int fragment_limit) {
    int frags = 0;
    if (surface_sub_)
        frags += surface_sub_->poll(surface_assembler_->handler(), fragment_limit);
    if (status_sub_) {
        frags +=
            status_sub_->poll([this](aeron::AtomicBuffer& buffer,
                                     aeron::util::index_t offset,
                                     aeron::util::index_t length,
                                     aeron::Header& header) { handle_status_fragment(buffer, offset, length, header); },
                              fragment_limit);
    }
    return frags;
}

void VolSurfaceClient::handle_surface_fragment(aeron::AtomicBuffer& buffer,
                                               aeron::util::index_t offset,
                                               aeron::util::index_t length,
                                               aeron::Header& /*header*/) {
    using namespace bpt::messages;

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

void VolSurfaceClient::handle_status_fragment(aeron::AtomicBuffer& buffer,
                                              aeron::util::index_t offset,
                                              aeron::util::index_t length,
                                              aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

    const auto tid = hdr.templateId();

    if (tid == PricerHeartbeat::sbeTemplateId()) {
        PricerHeartbeat hb;
        hb.wrapForDecode(data,
                         MessageHeader::encodedLength(),
                         hdr.blockLength(),
                         hdr.version(),
                         static_cast<uint64_t>(length));
        last_heartbeat_ns_ = hb.timestampNs();

    } else if (tid == PricerReady::sbeTemplateId()) {
        PricerReady ready;
        ready.wrapForDecode(data,
                            MessageHeader::encodedLength(),
                            hdr.blockLength(),
                            hdr.version(),
                            static_cast<uint64_t>(length));

        bpt::common::log::info("[VolSurfaceClient] PricerReady: exchanges=0x{:02x} underlyings={} points={}",
                       ready.exchangesLoaded(),
                       ready.underlyingCount(),
                       ready.pointCount());

        if (on_ready)
            on_ready(ready.exchangesLoaded(), ready.underlyingCount(), ready.pointCount());
    }
}

}  // namespace bpt::strategy::vol
