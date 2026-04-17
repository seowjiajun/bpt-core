#include "pricer/md/md_subscriber.h"

#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>

#include <chrono>
#include <thread>
#include <yggdrasil/logging.h>

namespace bpt::pricer::md {

MdSubscriber::MdSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int32_t stream_id) {
    const int64_t reg_id = aeron->addSubscription(channel, stream_id);
    for (int i = 0; i < 500; ++i) {
        sub_ = aeron->findSubscription(reg_id);
        if (sub_)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!sub_) {
        ygg::log::error("[MdSubscriber] Failed to find subscription on {} stream {}", channel, stream_id);
    } else {
        ygg::log::info("[MdSubscriber] Subscription ready on {} stream {}", channel, stream_id);
    }
}

int MdSubscriber::poll(int fragment_limit) {
    if (!sub_)
        return 0;
    return sub_->poll([this](const aeron::concurrent::AtomicBuffer& buffer,
                             aeron::util::index_t offset,
                             aeron::util::index_t length,
                             const aeron::Header& header) { on_fragment(buffer, offset, length, header); },
                      fragment_limit);
}

void MdSubscriber::on_fragment(const aeron::concurrent::AtomicBuffer& buffer,
                               aeron::util::index_t offset,
                               aeron::util::index_t length,
                               const aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
        return;

    MessageHeader hdr;
    hdr.wrap(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
             0,
             MessageHeader::sbeSchemaVersion(),
             static_cast<uint64_t>(length));

    const auto template_id = hdr.templateId();
    const auto hdr_len = MessageHeader::encodedLength();

    if (template_id == MdMarketData::sbeTemplateId()) {
        MdMarketData md;
        md.wrapForDecode(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
                         hdr_len,
                         hdr.blockLength(),
                         hdr.version(),
                         static_cast<uint64_t>(length));

        if (on_bbo_) {
            on_bbo_(md.instrumentId(), md.bidPrice(), md.askPrice(), md.timestampNs());
        }
    } else if (template_id == MdTrade::sbeTemplateId()) {
        MdTrade trade;
        trade.wrapForDecode(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
                            hdr_len,
                            hdr.blockLength(),
                            hdr.version(),
                            static_cast<uint64_t>(length));

        if (on_trade_) {
            on_trade_(trade.instrumentId(), trade.price(), trade.qty(), trade.timestampNs());
        }
    }
    // Ignore other message types (heartbeats, order book, etc.)
}

}  // namespace bpt::pricer::md
