#include "radar/messaging/subscribers/md_trade_subscriber.h"

#include <messages/MessageHeader.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::radar::messaging {

using bpt::messages::MdTrade;
using bpt::messages::MessageHeader;

MdTradeSubscriber::MdTradeSubscriber(std::shared_ptr<aeron::Aeron> aeron,
                                     const std::string& channel,
                                     int stream_id) {
    sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, stream_id);
    bpt::common::log::info("[MdTradeSubscriber] ready on stream {}", stream_id);
}

int MdTradeSubscriber::poll(int fragment_limit) {
    if (!sub_)
        return 0;
    return sub_->poll(
        [this](aeron::AtomicBuffer& buffer,
               aeron::util::index_t offset,
               aeron::util::index_t length,
               aeron::Header& header) { handle_fragment(buffer, offset, length, header); },
        fragment_limit);
}

void MdTradeSubscriber::handle_fragment(aeron::AtomicBuffer& buffer,
                                        aeron::util::index_t offset,
                                        aeron::util::index_t length,
                                        aeron::Header& /*header*/) {
    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

    // md_data shares stream 2002 with MdMarketData + MdOrderBook; ignore them.
    if (hdr.templateId() != MdTrade::sbeTemplateId())
        return;

    MdTrade msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<uint64_t>(length));

    if (on_trade)
        on_trade(msg);
}

}  // namespace bpt::radar::messaging
