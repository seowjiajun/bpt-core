#pragma once

/// \file
/// \brief Outbound port: ExecutionReport emission toward strategy.
///
/// Carved out of the concrete aeron::ExecReportPublisher so OrderProcessor
/// can be constructed in unit tests against a capturing or null
/// implementation without bringing up an Aeron MediaDriver.
///
/// Implementations: aeron::ExecReportPublisher in prod;
/// CapturingExecReportPublisher in unit tests.
///
/// publish() takes an ExecReport parameter object — the fields were
/// previously 15 positional args (seven of them uint64_t), a transposition
/// hazard. The struct is codec-free so the port stays SBE-agnostic.

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <cstdint>
#include <string>

namespace bpt::order_gateway::messaging::api {

struct ExecReport {
    uint64_t order_id;
    uint64_t exchange_order_id;
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id;
    bpt::messages::ExecStatus::Value status;
    bpt::messages::OrderSide::Value side;
    bpt::messages::OrderType::Value order_type;
    int64_t price;
    uint64_t filled_qty;
    uint64_t remaining_qty;
    bpt::messages::RejectReason::Value reject_reason;
    int64_t fee;
    std::string fee_currency;  ///< ≤ 8 chars; encoded into SBE Char8 slot
    uint64_t exchange_ts_ns;
    uint64_t local_ts_ns;
};

/// \brief Contract for the exec-report outbound port.
///
/// Called from the main poll thread in OrderProcessor on every exec
/// event drained from a venue adapter (ack, partial fill, fill, cancel,
/// reject) and on synthetic events (cancellations of stale orders,
/// risk-rejected NewOrders). Single-threaded contract — implementations
/// need not be thread-safe.
class ExecReportPublisher {
public:
    virtual ~ExecReportPublisher() = default;

    virtual void publish(const ExecReport& report) = 0;
};

}  // namespace bpt::order_gateway::messaging::api
