#include "order_gateway/messaging/subscribers/aeron/order_subscriber.h"

#include <messages/AccountSnapshotRequest.h>
#include <messages/CancelAll.h>
#include <messages/CancelOrder.h>
#include <messages/MessageHeader.h>
#include <messages/ModifyOrder.h>
#include <messages/NewOrder.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::order_gateway::messaging::aeron {

using bpt::messages::AccountSnapshotRequest;
using bpt::messages::CancelAll;
using bpt::messages::CancelOrder;
using bpt::messages::MessageHeader;
using bpt::messages::ModifyOrder;
using bpt::messages::NewOrder;

OrderSubscriber::OrderSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id) {
    subscription_ = std::make_unique<bpt::common::aeron::Subscriber>(
        std::move(aeron),
        channel,
        stream_id,
        [this](::aeron::AtomicBuffer& buf,
               ::aeron::util::index_t offset,
               ::aeron::util::index_t length,
               ::aeron::Header& hdr) { handle_fragment(buf, offset, length, hdr); });
}

void OrderSubscriber::handle_fragment(::aeron::AtomicBuffer& buf,
                                      ::aeron::util::index_t offset,
                                      ::aeron::util::index_t length,
                                      ::aeron::Header& /*hdr*/) {
    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buf.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    const uint16_t tmpl = hdr.templateId();

    if (tmpl == NewOrder::sbeTemplateId()) {
        NewOrder msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));
        if (on_new_order)
            on_new_order(msg);

    } else if (tmpl == CancelOrder::sbeTemplateId()) {
        CancelOrder msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));
        if (on_cancel)
            on_cancel(msg);

    } else if (tmpl == CancelAll::sbeTemplateId()) {
        CancelAll msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));
        if (on_cancel_all)
            on_cancel_all(msg);

    } else if (tmpl == ModifyOrder::sbeTemplateId()) {
        ModifyOrder msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));
        if (on_modify)
            on_modify(msg);

    } else if (tmpl == AccountSnapshotRequest::sbeTemplateId()) {
        AccountSnapshotRequest msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));
        if (on_account_snapshot_request)
            on_account_snapshot_request(msg);

    } else {
        bpt::common::log::warn("OrderSubscriber: unknown templateId={}", tmpl);
    }
}

int OrderSubscriber::poll(int fragment_limit) {
    return subscription_->poll(fragment_limit);
}

}  // namespace bpt::order_gateway::messaging::aeron
