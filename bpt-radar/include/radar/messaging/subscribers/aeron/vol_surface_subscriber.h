#pragma once

/// \file
/// Aeron concrete: subscribes to bpt-pricer's VolSurface stream.
/// CRTP-templated. Surface messages can span multiple fragments — uses
/// FragmentAssembler to reassemble.

#include "radar/messaging/subscribers/api/vol_surface_subscriber.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/MessageHeader.h>
#include <messages/VolSurface.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::radar::messaging::aeron {

template <class Handler>
class VolSurfaceSubscriber final : public api::VolSurfaceSubscriber {
public:
    VolSurfaceSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id) {
        sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, stream_id);
        assembler_ = std::make_unique<::aeron::FragmentAssembler>(
            [this](::aeron::AtomicBuffer& buffer,
                   ::aeron::util::index_t offset,
                   ::aeron::util::index_t length,
                   ::aeron::Header& header) { handle_fragment(buffer, offset, length, header); });

        bpt::common::log::info("[VolSurfaceSubscriber] ready on stream {}", stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 4) override {
        if (!sub_)
            return 0;
        return sub_->poll(assembler_->handler(), fragment_limit);
    }

private:
    void handle_fragment(::aeron::AtomicBuffer& buffer,
                         ::aeron::util::index_t offset,
                         ::aeron::util::index_t length,
                         ::aeron::Header& /*header*/) {
        using bpt::messages::MessageHeader;
        using bpt::messages::VolSurface;

        if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
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

        if (handler_ != nullptr) [[likely]]
            handler_->on_vol_surface(msg);
    }

    std::shared_ptr<::aeron::Subscription> sub_;
    std::unique_ptr<::aeron::FragmentAssembler> assembler_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::radar::messaging::aeron
