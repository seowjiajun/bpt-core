#include "strategy/md/md_client.h"

#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdServiceHeartbeat.h>
#include <messages/MdSubscribeBatch.h>
#include <messages/MdSubscriptionAck.h>
#include <messages/MdSubscriptionHeartbeat.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>

#include <cstring>
#include <x86intrin.h>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>
#include <yggdrasil/util/tsc_clock.h>

namespace bpt::strategy::md {

MdClient::MdClient(std::shared_ptr<aeron::Aeron> aeron,
                   const std::string& channel,
                   int control_stream,
                   int data_stream,
                   int ack_hb_stream) {
    ctrl_pub_ = ygg::aeron::wait_for_publication(aeron, channel, control_stream);
    data_sub_ = ygg::aeron::wait_for_subscription(aeron, channel, data_stream);
    ack_hb_sub_ = ygg::aeron::wait_for_subscription(aeron, channel, ack_hb_stream);

    data_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t offset, aeron::util::index_t length, aeron::Header& hdr) {
            handle_data_fragment(buf, offset, length, hdr);
        });

    ygg::log::info("MdClient connected: ctrl={} data={} ack_hb={}", control_stream, data_stream, ack_hb_stream);
}

void MdClient::subscribe(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) {
    using namespace bpt::messages;

    const auto n = static_cast<uint16_t>(instruments.size());

    std::size_t buf_size = MessageHeader::encodedLength() + MdSubscribeBatch::sbeBlockLength() +
                           MdSubscribeBatch::Instruments::sbeHeaderSize() +
                           n * MdSubscribeBatch::Instruments::sbeBlockLength();

    std::vector<char> buf(buf_size, '\0');

    MdSubscribeBatch msg;
    msg.wrapAndApplyHeader(buf.data(), 0, buf_size)
        .correlationId(correlation_id)
        .timestampNs(ygg::util::TscClock::now_epoch_ns());

    auto& g = msg.instrumentsCount(n);
    for (const auto& inst : instruments) {
        g.next()
            .instrumentId(inst.instrument_id)
            .putExchange(inst.exchange.c_str())
            .putSymbol(inst.symbol.c_str())
            .depth(inst.depth);
    }

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf.data()), static_cast<aeron::util::index_t>(buf_size));

    while (ctrl_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(buf_size)) < 0)
        _mm_pause();

    ygg::log::info("MdClient: subscription sent correlation_id={} instruments={}", correlation_id, n);
}

void MdClient::handle_data_fragment(aeron::AtomicBuffer& buffer,
                                    aeron::util::index_t offset,
                                    aeron::util::index_t length,
                                    aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    static uint64_t frag_count = 0;
    if (++frag_count <= 5) {
        ygg::log::info("[MdClient] fragment: templateId={} expected_bbo={} length={}",
                       hdr.templateId(),
                       MdMarketData::sbeTemplateId(),
                       length);
    }

    switch (hdr.templateId()) {
        case MdMarketData::sbeTemplateId(): {
            MdMarketData msg;
            msg.wrapForDecode(data,
                              MessageHeader::encodedLength(),
                              hdr.blockLength(),
                              hdr.version(),
                              static_cast<std::size_t>(length));
            if (on_bbo)
                on_bbo(msg);
            break;
        }
        case MdTrade::sbeTemplateId(): {
            MdTrade msg;
            msg.wrapForDecode(data,
                              MessageHeader::encodedLength(),
                              hdr.blockLength(),
                              hdr.version(),
                              static_cast<std::size_t>(length));
            if (on_trade)
                on_trade(msg);
            break;
        }
        case MdOrderBook::sbeTemplateId(): {
            MdOrderBook msg;
            msg.wrapForDecode(data,
                              MessageHeader::encodedLength(),
                              hdr.blockLength(),
                              hdr.version(),
                              static_cast<std::size_t>(length));
            if (on_order_book)
                on_order_book(msg);
            break;
        }
        default:
            break;
    }
}

void MdClient::handle_ack_hb_fragment(aeron::AtomicBuffer& buffer,
                                      aeron::util::index_t offset,
                                      aeron::util::index_t length,
                                      aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() == MdServiceHeartbeat::sbeTemplateId()) {
        MdServiceHeartbeat msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));
        last_service_hb_ns_ = msg.timestampNs();
        if (on_service_heartbeat)
            on_service_heartbeat();

    } else if (hdr.templateId() == MdSubscriptionAck::sbeTemplateId()) {
        MdSubscriptionAck msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));
        ygg::log::info("MdClient: ack instrument_id={} status={}",
                       msg.instrumentId(),
                       static_cast<int>(msg.ackStatus()));
    }
    // MdSubscriptionHeartbeat silently consumed — used by a watchdog if needed
}

int MdClient::poll(int fragment_limit) {
    int total = 0;

    int data_frags = data_sub_->poll(data_assembler_->handler(), fragment_limit);
    total += data_frags;

    static uint64_t data_poll_count = 0;
    static uint64_t total_data_frags = 0;
    total_data_frags += data_frags;
    if (++data_poll_count % 100000 == 0) {
        ygg::log::info("[MdClient] poll stats: polls={} data_frags_total={} connected={}",
                       data_poll_count,
                       total_data_frags,
                       data_sub_->isConnected());
    }

    total += ack_hb_sub_->poll(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t offset, aeron::util::index_t length, aeron::Header& hdr) {
            handle_ack_hb_fragment(buf, offset, length, hdr);
        },
        fragment_limit);

    return total;
}

}  // namespace bpt::strategy::md
