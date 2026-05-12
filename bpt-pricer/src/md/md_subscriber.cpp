#include "pricer/md/md_subscriber.h"

#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>

#include <bpt_common/logging.h>

namespace bpt::pricer::md {

MdSubscriber::MdSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id) {
    sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
        std::move(aeron),
        channel,
        stream_id,
        [this](::aeron::AtomicBuffer& buf,
               ::aeron::util::index_t offset,
               ::aeron::util::index_t length,
               ::aeron::Header& hdr) { on_fragment(buf, offset, length, hdr); });
    bpt::common::log::info("[MdSubscriber] Subscription ready on {} stream {}", channel, stream_id);
}

int MdSubscriber::poll(int fragment_limit) {
    return sub_ ? sub_->poll(fragment_limit) : 0;
}

void MdSubscriber::on_fragment(::aeron::AtomicBuffer& buffer,
                               ::aeron::util::index_t offset,
                               ::aeron::util::index_t length,
                               ::aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
        return;

    auto* data = reinterpret_cast<char*>(buffer.buffer() + offset);
    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

    const auto template_id = hdr.templateId();
    const auto hdr_len = MessageHeader::encodedLength();

    if (template_id == MdMarketData::sbeTemplateId()) {
        MdMarketData md;
        md.wrapForDecode(data, hdr_len, hdr.blockLength(), hdr.version(), static_cast<uint64_t>(length));
        if (on_bbo_)
            on_bbo_(md.instrumentId(), md.bidPrice(), md.askPrice(), md.timestampNs());
    } else if (template_id == MdTrade::sbeTemplateId()) {
        MdTrade trade;
        trade.wrapForDecode(data, hdr_len, hdr.blockLength(), hdr.version(), static_cast<uint64_t>(length));
        if (on_trade_)
            on_trade_(trade.instrumentId(), trade.price(), trade.qty(), trade.timestampNs());
    }
}

}  // namespace bpt::pricer::md
