#pragma once

/// \file
/// Aeron concrete: subscribes to bpt-md-gateway's md_data stream and
/// fans out MdMarketData (BBO) fragments only. CRTP-templated.

#include "radar/messaging/subscribers/api/md_market_data_subscriber.h"

#include <Aeron.h>

#include <messages/MdMarketData.h>
#include <messages/MessageHeader.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::radar::messaging::aeron {

template <class Handler>
class MdMarketDataSubscriber final : public api::MdMarketDataSubscriber {
public:
    MdMarketDataSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id) {
        sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, stream_id);
        bpt::common::log::info("[MdMarketDataSubscriber] ready on stream {}", stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 16) override {
        if (!sub_)
            return 0;
        return sub_->poll([this](::aeron::AtomicBuffer& buffer,
                                 ::aeron::util::index_t offset,
                                 ::aeron::util::index_t length,
                                 ::aeron::Header& header) { handle_fragment(buffer, offset, length, header); },
                          fragment_limit);
    }

private:
    void handle_fragment(::aeron::AtomicBuffer& buffer,
                         ::aeron::util::index_t offset,
                         ::aeron::util::index_t length,
                         ::aeron::Header& /*header*/) {
        using bpt::messages::MdMarketData;
        using bpt::messages::MessageHeader;

        if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
            return;

        char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
        MessageHeader hdr;
        hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

        if (hdr.templateId() != MdMarketData::sbeTemplateId())
            return;

        MdMarketData msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<uint64_t>(length));

        if (handler_ != nullptr) [[likely]]
            handler_->on_bbo(msg);
    }

    std::shared_ptr<::aeron::Subscription> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::radar::messaging::aeron
