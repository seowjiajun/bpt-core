#include "bridge/md_subscriber.h"

#include <messages/MdMarketData.h>
#include <messages/MessageHeader.h>
#include <chrono>
#include <thread>
#include <yggdrasil/logging.h>

namespace bridge {

MdSubscriber::MdSubscriber(std::shared_ptr<aeron::Aeron> aeron,
                           const std::string& channel,
                           int32_t stream_id) {
    const int64_t reg_id = aeron->addSubscription(channel, stream_id);
    for (int i = 0; i < 500; ++i) {
        sub_ = aeron->findSubscription(reg_id);
        if (sub_) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!sub_) {
        ygg::log::error("[bridge/MD] failed to register subscription on {} stream {}", channel, stream_id);
    } else {
        ygg::log::info("[bridge/MD] subscribed on {} stream {}", channel, stream_id);
    }
}

int MdSubscriber::poll(int fragment_limit) {
    if (!sub_) return 0;
    return sub_->poll(
        [this](const aeron::concurrent::AtomicBuffer& b,
               aeron::util::index_t o,
               aeron::util::index_t l,
               const aeron::Header& h) { on_fragment(b, o, l, h); },
        fragment_limit);
}

void MdSubscriber::on_fragment(const aeron::concurrent::AtomicBuffer& buffer,
                               aeron::util::index_t offset,
                               aeron::util::index_t length,
                               const aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength())) return;

    MessageHeader hdr;
    hdr.wrap(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
             0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

    if (hdr.templateId() != MdMarketData::sbeTemplateId()) return;

    MdMarketData md;
    md.wrapForDecode(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
                     MessageHeader::encodedLength(),
                     hdr.blockLength(),
                     hdr.version(),
                     static_cast<uint64_t>(length));

    const double bid = md.bidPrice();
    const double ask = md.askPrice();
    if (bid <= 0 || ask <= 0) return;
    const double mid = (bid + ask) * 0.5;

    if (handler_) handler_(md.instrumentId(), mid, md.timestampNs());
}

}  // namespace bridge
