#include "order_gateway/messaging/publishers/aeron/exec_report_publisher.h"

#include <cstddef>

namespace bpt::order_gateway::messaging::aeron {

using Policy = bpt::common::aeron::Publisher::Policy;

ExecReportPublisher::ExecReportPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                         const std::string& channel,
                                         int stream_id)
    // Exec reports must reach the strategy — they drive position
    // tracking. Spin through back-pressure; drop if no subscriber
    // rather than hang (strategy down → gateway isn't useful anyway).
    : publisher_(std::move(aeron), channel, stream_id, Policy::kRetryOnBackpressure) {}

void ExecReportPublisher::publish(uint64_t order_id,
                                  uint64_t exchange_order_id,
                                  bpt::messages::ExchangeId::Value exchange_id,
                                  uint64_t instrument_id,
                                  bpt::messages::ExecStatus::Value status,
                                  bpt::messages::OrderSide::Value side,
                                  bpt::messages::OrderType::Value order_type,
                                  int64_t price,
                                  uint64_t filled_qty,
                                  uint64_t remaining_qty,
                                  bpt::messages::RejectReason::Value reject_reason,
                                  int64_t fee,
                                  std::string_view fee_currency,
                                  uint64_t exchange_ts_ns,
                                  uint64_t local_ts_ns) {
    alignas(8) std::byte scratch[SbeExecutionReportCodec::kRecommendedScratchSize];
    ExecutionReportMsg m{
        .order_id = order_id,
        .exchange_order_id = exchange_order_id,
        .exchange_id = exchange_id,
        .instrument_id = instrument_id,
        .status = status,
        .side = side,
        .order_type = order_type,
        .price = price,
        .filled_qty = filled_qty,
        .remaining_qty = remaining_qty,
        .reject_reason = reject_reason,
        .fee = fee,
        .fee_currency = std::string(fee_currency),
        .exchange_ts_ns = exchange_ts_ns,
        .local_ts_ns = local_ts_ns,
    };
    const auto bytes = codec_.encode(m, scratch);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(bytes.size()));
}

}  // namespace bpt::order_gateway::messaging::aeron
