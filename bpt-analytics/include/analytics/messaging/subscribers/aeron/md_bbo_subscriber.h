#pragma once

/// @file
/// Aeron-backed concrete for api::MdBboSubscriber. Templated on the
/// Handler — in prod the Handler is `AnalyticsService` and the
/// per-tick path is direct (no std::function indirection).

#include "analytics/messaging/subscribers/api/md_bbo_subscriber.h"

#include <messages/MdMarketData.h>
#include <messages/MessageHeader.h>

#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bpt::analytics::messaging::aeron {

template <class Handler>
class MdBboSubscriber final : public api::MdBboSubscriber {
public:
    MdBboSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream)
        : sub_(std::make_unique<bpt::common::aeron::Subscriber>(
              std::move(aeron),
              stream.channel,
              stream.stream_id,
              [this](::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& /*hdr*/) {
                  using bpt::messages::MdMarketData;
                  using bpt::messages::MessageHeader;

                  if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
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
                  if (handler_ != nullptr) [[likely]]
                      handler_->on_bbo(md.instrumentId(), md.bidPrice(), md.askPrice(), md.timestampNs());
              })) {}

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 10) override { return sub_ ? sub_->poll(fragment_limit) : 0; }

private:
    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::analytics::messaging::aeron
