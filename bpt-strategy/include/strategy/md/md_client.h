#pragma once

/// @file
/// AeronMdClient — Aeron-backed implementation of IMdClient. Templated on
/// the Handler type that receives parsed messages — in prod the Handler
/// is `StrategyService` and the optimiser sees the full chain (fragment
/// → handler) without a single vtable hop or std::function indirection.
///
/// Publishes MdSubscribeBatch on the control stream, subscribes to
/// MdMarketData + MdTrade + MdOrderBook on the data stream, and
/// subscribes to acks + heartbeats on the ack/hb stream.
///
/// `MdClient` remains as a deprecated alias for AeronMdClient<H> so
/// existing call sites compile during the transition.
///
/// Single-threaded: only call from the strategy poll thread. `handler_`
/// is set once at startup via `set_handler()` and stays put for the
/// life of the client.

#include "strategy/md/i_md_client.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/AckStatus.h>
#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdServiceHeartbeat.h>
#include <messages/MdSubscribeBatch.h>
#include <messages/MdSubscriptionAck.h>
#include <messages/MdSubscriptionHeartbeat.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>
#include <messages/TradeSide.h>

#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/subscriber.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bpt::strategy::md {

template <class Handler>
class AeronMdClient : public IMdClient {
public:
    AeronMdClient(std::shared_ptr<aeron::Aeron> aeron,
                  const std::string& channel,
                  int control_stream,
                  int data_stream,
                  int ack_hb_stream) {
        ctrl_pub_ = std::make_unique<bpt::common::aeron::Publisher>(
            aeron, channel, control_stream, bpt::common::aeron::Publisher::Policy::kRetryOnBackpressure);
        data_sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            aeron,
            channel,
            data_stream,
            [this](aeron::AtomicBuffer& buf,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& hdr) { handle_data_fragment(buf, offset, length, hdr); });
        ack_hb_sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            aeron,
            channel,
            ack_hb_stream,
            [this](aeron::AtomicBuffer& buf,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& hdr) { handle_ack_hb_fragment(buf, offset, length, hdr); });

        bpt::common::log::info(
            "MdClient connected: ctrl={} data={} ack_hb={}", control_stream, data_stream, ack_hb_stream);
    }

    /// Bind the per-tick dispatch target. Called once during StrategyService
    /// construction; must be set before the first poll() to avoid
    /// dispatching into a null handler.
    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    void subscribe(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) override {
        using bpt::messages::MdSubscribeBatch;
        using bpt::messages::MessageHeader;

        const auto n = static_cast<uint16_t>(instruments.size());

        std::size_t buf_size = MessageHeader::encodedLength() + MdSubscribeBatch::sbeBlockLength() +
                               MdSubscribeBatch::Instruments::sbeHeaderSize() +
                               n * MdSubscribeBatch::Instruments::sbeBlockLength();

        std::vector<char> buf(buf_size, '\0');

        MdSubscribeBatch msg;
        msg.wrapAndApplyHeader(buf.data(), 0, buf_size)
            .correlationId(correlation_id)
            .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

        auto& g = msg.instrumentsCount(n);
        for (const auto& inst : instruments) {
            g.next()
                .instrumentId(inst.instrument_id)
                .putExchange(inst.exchange.c_str())
                .putSymbol(inst.symbol.c_str())
                .depth(inst.depth);
        }

        aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf.data()), static_cast<aeron::util::index_t>(buf_size));

        ctrl_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(buf_size));

        bpt::common::log::info(
            "MdClient: subscription sent correlation_id={} instruments={}", correlation_id, n);
    }

    int poll(int fragment_limit = 10) override {
        int total = 0;

        int data_frags = data_sub_->poll(fragment_limit);
        total += data_frags;

        static uint64_t data_poll_count = 0;
        static uint64_t total_data_frags = 0;
        total_data_frags += data_frags;
        if (++data_poll_count % 100000 == 0) {
            bpt::common::log::info("[MdClient] poll stats: polls={} data_frags_total={} connected={}",
                                   data_poll_count,
                                   total_data_frags,
                                   data_sub_->is_connected());
        }

        total += ack_hb_sub_->poll(fragment_limit);
        return total;
    }

    /// Nanosecond timestamp of the last MdServiceHeartbeat (0 if none yet).
    [[nodiscard]] uint64_t last_service_heartbeat_ns() const noexcept { return last_service_hb_ns_; }

private:
    void handle_data_fragment(aeron::AtomicBuffer& buffer,
                              aeron::util::index_t offset,
                              aeron::util::index_t length,
                              aeron::Header& /*header*/) {
        using bpt::messages::MdMarketData;
        using bpt::messages::MdOrderBook;
        using bpt::messages::MdTrade;
        using bpt::messages::MessageHeader;

        if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
            return;

        char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
        MessageHeader hdr(data, static_cast<std::size_t>(length));

        static uint64_t frag_count = 0;
        if (++frag_count <= 5) {
            bpt::common::log::info("[MdClient] fragment: templateId={} expected_bbo={} length={}",
                                   hdr.templateId(),
                                   MdMarketData::sbeTemplateId(),
                                   length);
        }

        if (handler_ == nullptr) [[unlikely]]
            return;

        switch (hdr.templateId()) {
            case MdMarketData::sbeTemplateId(): {
                MdMarketData msg;
                msg.wrapForDecode(data,
                                  MessageHeader::encodedLength(),
                                  hdr.blockLength(),
                                  hdr.version(),
                                  static_cast<std::size_t>(length));
                handler_->on_bbo(msg);
                break;
            }
            case MdTrade::sbeTemplateId(): {
                MdTrade msg;
                msg.wrapForDecode(data,
                                  MessageHeader::encodedLength(),
                                  hdr.blockLength(),
                                  hdr.version(),
                                  static_cast<std::size_t>(length));
                handler_->on_trade(msg);
                break;
            }
            case MdOrderBook::sbeTemplateId(): {
                MdOrderBook msg;
                msg.wrapForDecode(data,
                                  MessageHeader::encodedLength(),
                                  hdr.blockLength(),
                                  hdr.version(),
                                  static_cast<std::size_t>(length));
                handler_->on_order_book(msg);
                break;
            }
            default:
                break;
        }
    }

    void handle_ack_hb_fragment(aeron::AtomicBuffer& buffer,
                                aeron::util::index_t offset,
                                aeron::util::index_t length,
                                aeron::Header& /*header*/) {
        using bpt::messages::MdServiceHeartbeat;
        using bpt::messages::MdSubscriptionAck;
        using bpt::messages::MessageHeader;

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
            if (handler_ != nullptr) [[likely]]
                handler_->on_md_service_heartbeat();

        } else if (hdr.templateId() == MdSubscriptionAck::sbeTemplateId()) {
            MdSubscriptionAck msg;
            msg.wrapForDecode(data,
                              MessageHeader::encodedLength(),
                              hdr.blockLength(),
                              hdr.version(),
                              static_cast<std::size_t>(length));
            bpt::common::log::info("MdClient: ack instrument_id={} status={}",
                                   msg.instrumentId(),
                                   static_cast<int>(msg.ackStatus()));
        }
        // MdSubscriptionHeartbeat silently consumed — used by a watchdog if needed
    }

    std::unique_ptr<bpt::common::aeron::Publisher> ctrl_pub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> data_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> ack_hb_sub_;
    Handler* handler_{nullptr};
    uint64_t last_service_hb_ns_{0};
};

}  // namespace bpt::strategy::md
