#pragma once

/// \file
/// \brief Outbound port: ExecutionReport emission toward strategy.
///
/// Carved out of the concrete ExecReportPublisher so OrderProcessor can
/// be constructed in unit tests against a capturing or null
/// implementation without bringing up an Aeron MediaDriver.
///
/// Implementations: ExecReportPublisher (Aeron-backed) in prod;
/// CapturingExecReportPublisher in unit tests.
///
/// The 15-argument publish() signature is preserved verbatim from the
/// pre-port-extraction ExecReportPublisher. Callers upcast the concrete
/// instance to `IExecReportPublisher&` at the OrderProcessor boundary.

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <cstdint>
#include <string_view>

namespace bpt::order_gateway::messaging {

/// \brief Contract for the exec-report outbound port.
///
/// Called from the main poll thread in OrderProcessor on every exec
/// event drained from a venue adapter (ack, partial fill, fill, cancel,
/// reject) and on synthetic events (cancellations of stale orders,
/// risk-rejected NewOrders). Single-threaded contract — implementations
/// need not be thread-safe.
class IExecReportPublisher {
public:
    virtual ~IExecReportPublisher() = default;

    virtual void publish(uint64_t order_id,
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
                         uint64_t local_ts_ns) = 0;
};

}  // namespace bpt::order_gateway::messaging
