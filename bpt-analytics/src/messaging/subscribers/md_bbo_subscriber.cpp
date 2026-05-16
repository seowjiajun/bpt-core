#include "analytics/messaging/subscribers/md_bbo_subscriber.h"

#include <messages/MdMarketData.h>
#include <messages/MessageHeader.h>

namespace bpt::analytics::messaging {

using bpt::messages::MdMarketData;
using bpt::messages::MessageHeader;

MdBboSubscriber::MdBboSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id)
    : sub_(std::make_unique<bpt::common::aeron::Subscriber>(
          std::move(aeron),
          channel,
          stream_id,
          [this](aeron::AtomicBuffer& buffer,
                 aeron::util::index_t offset,
                 aeron::util::index_t length,
                 aeron::Header& /*hdr*/) {
              if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
                  return;

              auto* data = reinterpret_cast<char*>(buffer.buffer() + offset);
              MessageHeader hdr;
              hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));
              if (hdr.templateId() != MdMarketData::sbeTemplateId())
                  return;

              MdMarketData md;
              md.wrapForDecode(data,
                               MessageHeader::encodedLength(),
                               hdr.blockLength(),
                               hdr.version(),
                               static_cast<uint64_t>(length));
              if (on_bbo)
                  on_bbo(md.instrumentId(), md.bidPrice(), md.askPrice(), md.timestampNs());
          })) {}

int MdBboSubscriber::poll(int fragment_limit) {
    return sub_ ? sub_->poll(fragment_limit) : 0;
}

}  // namespace bpt::analytics::messaging
