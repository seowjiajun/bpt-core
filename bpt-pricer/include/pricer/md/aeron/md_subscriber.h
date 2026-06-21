#pragma once

/// \file
/// Aeron-backed concrete for api::MdSubscriber. Templated on the
/// Handler — in prod the Handler is `PricerService` and the per-tick
/// dispatch path is direct (no std::function indirection).

#include "pricer/md/api/md_subscriber.h"

#include <Aeron.h>

#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>

#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/aeron/subscriber.h>
#include <bpt_common/logging.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bpt::pricer::md::aeron {

template <class Handler>
class MdSubscriber final : public api::MdSubscriber {
public:
    MdSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream) {
        sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            std::move(aeron),
            stream.channel,
            stream.stream_id,
            [this](::aeron::AtomicBuffer& buf,
                   ::aeron::util::index_t offset,
                   ::aeron::util::index_t length,
                   ::aeron::Header& hdr) { on_fragment(buf, offset, length, hdr); });
        bpt::common::log::info("[MdSubscriber] Subscription ready on {} stream {}", stream.channel, stream.stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 10) override { return sub_ ? sub_->poll(fragment_limit) : 0; }

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& /*header*/) {
        using bpt::messages::MdMarketData;
        using bpt::messages::MdTrade;
        using bpt::messages::MessageHeader;

        if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
            return;

        auto* data = reinterpret_cast<char*>(buffer.buffer() + offset);
        MessageHeader hdr;
        hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

        const auto template_id = hdr.templateId();
        const auto hdr_len = MessageHeader::encodedLength();

        if (handler_ == nullptr) [[unlikely]]
            return;

        if (template_id == MdMarketData::sbeTemplateId()) {
            MdMarketData md;
            md.wrapForDecode(data, hdr_len, hdr.blockLength(), hdr.version(), static_cast<uint64_t>(length));
            handler_->on_bbo(md.instrumentId(), md.bidPrice(), md.askPrice(), md.timestampNs());
        } else if (template_id == MdTrade::sbeTemplateId()) {
            MdTrade trade;
            trade.wrapForDecode(data, hdr_len, hdr.blockLength(), hdr.version(), static_cast<uint64_t>(length));
            handler_->on_trade(trade.instrumentId(), trade.price(), trade.qty(), trade.timestampNs());
        }
    }

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::pricer::md::aeron
