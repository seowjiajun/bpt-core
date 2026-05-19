#pragma once

/// @file
/// Aeron-backed concrete for api::ExecReportSubscriber. Templated on the
/// Handler — in prod the Handler is `AnalyticsService`.

#include "analytics/messaging/subscribers/api/exec_report_subscriber.h"

#include <messages/ExecutionReport.h>
#include <messages/MessageHeader.h>

#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bpt::analytics::messaging::aeron {

template <class Handler>
class ExecReportSubscriber final : public api::ExecReportSubscriber {
public:
    ExecReportSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id)
        : sub_(std::make_unique<bpt::common::aeron::Subscriber>(
              std::move(aeron),
              channel,
              stream_id,
              [this](::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& /*hdr*/) {
                  using bpt::messages::ExecutionReport;
                  using bpt::messages::MessageHeader;

                  if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
                      return;

                  auto* data = reinterpret_cast<char*>(buffer.buffer() + offset);
                  MessageHeader hdr;
                  hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));
                  if (hdr.templateId() != ExecutionReport::sbeTemplateId())
                      return;

                  ExecutionReport rpt;
                  rpt.wrapForDecode(data,
                                    MessageHeader::encodedLength(),
                                    hdr.blockLength(),
                                    hdr.version(),
                                    static_cast<uint64_t>(length));
                  if (handler_ != nullptr) [[likely]]
                      handler_->on_exec_report(rpt);
              })) {}

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 10) override { return sub_ ? sub_->poll(fragment_limit) : 0; }

private:
    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::analytics::messaging::aeron
