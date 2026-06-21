#pragma once

#include "order_gateway/messaging/codecs/sbe_execution_report_codec.h"
#include "order_gateway/messaging/publishers/api/exec_report_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/stream_config.h>
#include <memory>
#include <string>

namespace bpt::order_gateway::messaging::aeron {

/// \brief Aeron-backed concrete for api::ExecReportPublisher.
///
/// Publishes ExecutionReport (SBE) on the exec-report stream toward
/// strategy. Back-pressure policy is `kRetryOnBackpressure` — exec
/// reports drive position tracking, so we'd rather block briefly than
/// drop them. If no subscriber is connected at all, drop instead of
/// hang (strategy down → gateway can't be useful anyway).
class ExecReportPublisher final : public api::ExecReportPublisher {
public:
    ExecReportPublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    void publish(const api::ExecReport& report) override;

private:
    bpt::common::aeron::Publisher publisher_;
    SbeExecutionReportCodec codec_;
};

}  // namespace bpt::order_gateway::messaging::aeron
