#pragma once

#include "order_gateway/messaging/i_exec_report_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <memory>
#include <string>

namespace bpt::order_gateway::messaging {

/// \brief Aeron-backed concrete for IExecReportPublisher.
///
/// Publishes ExecutionReport (SBE) on the exec-report stream toward
/// strategy. Back-pressure policy is `kRetryOnBackpressure` — exec
/// reports drive position tracking, so we'd rather block briefly than
/// drop them. If no subscriber is connected at all, drop instead of
/// hang (strategy down → gateway can't be useful anyway).
class ExecReportPublisher final : public IExecReportPublisher {
public:
    ExecReportPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(uint64_t order_id,
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
                 uint64_t local_ts_ns) override;

private:
    bpt::common::aeron::Publisher publisher_;
};

}  // namespace bpt::order_gateway::messaging
