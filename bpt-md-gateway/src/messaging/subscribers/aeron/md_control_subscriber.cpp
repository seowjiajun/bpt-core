#include "md_gateway/messaging/subscribers/aeron/md_control_subscriber.h"

#include <messages/MdSubscribeBatch.h>
#include <messages/MessageHeader.h>

#include <bpt_common/logging.h>
#include <cstddef>

namespace bpt::md_gateway::messaging::aeron {

using bpt::messages::MdSubscribeBatch;
using bpt::messages::MessageHeader;

MdControlSubscriber::MdControlSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                         const bpt::common::config::StreamConfig& stream) {
    subscription_ = std::make_unique<bpt::common::aeron::Subscriber>(
        std::move(aeron),
        stream.channel,
        stream.stream_id,
        [this](::aeron::AtomicBuffer& buf,
               ::aeron::util::index_t offset,
               ::aeron::util::index_t length,
               ::aeron::Header& /*hdr*/) {
            if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
                return;

            char* data = reinterpret_cast<char*>(buf.buffer()) + offset;
            MessageHeader hdr(data, static_cast<std::size_t>(length));

            if (hdr.templateId() != MdSubscribeBatch::sbeTemplateId())
                return;

            MdSubscribeBatch msg;
            msg.wrapForDecode(data,
                              MessageHeader::encodedLength(),
                              hdr.blockLength(),
                              hdr.version(),
                              static_cast<std::size_t>(length));

            if (current_handler_)
                current_handler_(msg);
        });
}

int MdControlSubscriber::poll(BatchHandler handler) {
    if (!subscription_->is_connected())
        return 0;

    current_handler_ = std::move(handler);
    int fragments = subscription_->poll(10);
    current_handler_ = nullptr;
    return fragments;
}

}  // namespace bpt::md_gateway::messaging::aeron
