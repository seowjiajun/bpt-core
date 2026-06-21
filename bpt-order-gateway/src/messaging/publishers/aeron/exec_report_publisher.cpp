#include "order_gateway/messaging/publishers/aeron/exec_report_publisher.h"

#include <cstddef>

namespace bpt::order_gateway::messaging::aeron {

using Policy = bpt::common::aeron::Publisher::Policy;

ExecReportPublisher::ExecReportPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                         const bpt::common::config::StreamConfig& stream)
    // Exec reports must reach the strategy — they drive position
    // tracking. Spin through back-pressure; drop if no subscriber
    // rather than hang (strategy down → gateway isn't useful anyway).
    : publisher_(std::move(aeron), stream.channel, stream.stream_id, Policy::kRetryOnBackpressure) {}

void ExecReportPublisher::publish(const api::ExecReport& report) {
    alignas(8) std::byte scratch[SbeExecutionReportCodec::kRecommendedScratchSize];
    const auto bytes = codec_.encode(report, scratch);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(bytes.size()));
}

}  // namespace bpt::order_gateway::messaging::aeron
