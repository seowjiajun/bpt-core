#pragma once

/// \file
/// Aeron concrete: subscribes to bpt-md-gateway's FundingRate stream
/// (typically 2005). CRTP-templated on the Handler — in prod the
/// Handler is `RadarService` and per-frame dispatch is direct (no
/// std::function indirection).

#include "radar/messaging/subscribers/api/funding_rate_subscriber.h"

#include <Aeron.h>

#include <messages/FundingRate.h>
#include <messages/MessageHeader.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/logging.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::radar::messaging::aeron {

template <class Handler>
class FundingRateSubscriber final : public api::FundingRateSubscriber {
public:
    FundingRateSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream) {
        sub_ = bpt::common::aeron::wait_for_subscription(aeron, stream.channel, stream.stream_id);
        bpt::common::log::info("[FundingRateSubscriber] ready on stream {}", stream.stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 8) override {
        if (!sub_)
            return 0;
        return sub_->poll([this](::aeron::AtomicBuffer& buffer,
                                 ::aeron::util::index_t offset,
                                 ::aeron::util::index_t length,
                                 ::aeron::Header& header) { handle_fragment(buffer, offset, length, header); },
                          fragment_limit);
    }

private:
    void handle_fragment(::aeron::AtomicBuffer& buffer,
                         ::aeron::util::index_t offset,
                         ::aeron::util::index_t length,
                         ::aeron::Header& /*header*/) {
        using bpt::messages::FundingRate;
        using bpt::messages::MessageHeader;

        if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
            return;

        char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
        MessageHeader hdr;
        hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

        if (hdr.templateId() != FundingRate::sbeTemplateId())
            return;

        FundingRate msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<uint64_t>(length));

        if (handler_ != nullptr) [[likely]]
            handler_->on_funding(msg);
    }

    std::shared_ptr<::aeron::Subscription> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::radar::messaging::aeron
