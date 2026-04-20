#pragma once

// Abstract base for any "order gateway" the strategy talks to. Two
// concrete impls live alongside:
//
//   AeronOrderGatewayClient — production path; publishes NewOrder /
//     CancelOrder / ModifyOrder messages to bpt-order-gateway over
//     Aeron and consumes the exec-report / heartbeat / account-snapshot
//     streams it publishes back.
//
//   PaperOrderGatewayClient — canary / shadow path; swallows every
//     order locally, runs a simple fill engine against the MD feed,
//     and synthesises ExecutionReports back through on_exec_report so
//     the strategy's normal order-lifecycle machinery fires unchanged.
//
// The interface is kept narrow on purpose — everything the strategy
// actually does with the gateway (send, cancel, modify, poll, snapshot
// request) is here; Aeron details live only in the Aeron impl.

#include <messages/AccountSnapshot.h>
#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/OrderGatewayHeartbeat.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <cstdint>
#include <functional>
#include <string>

namespace bpt::strategy::order {

class IOrderGatewayClient {
public:
    using OnExecReportFn = std::function<void(const bpt::messages::ExecutionReport&)>;
    using OnHeartbeatFn = std::function<void(const bpt::messages::OrderGatewayHeartbeat&)>;
    using OnAccountSnapshotFn = std::function<void(bpt::messages::AccountSnapshot&)>;

    virtual ~IOrderGatewayClient() = default;

    // Send a new order. See AeronOrderGatewayClient for validation rules
    // (non-zero quantity, positive price for LIMIT, non-empty symbol).
    // Returns false if pre-flight checks fail; the caller must undo any
    // bookkeeping tied to the rejected order.
    [[nodiscard]] virtual bool send_new_order(uint64_t order_id,
                                              bpt::messages::ExchangeId::Value exchange_id,
                                              uint64_t instrument_id,
                                              bpt::messages::OrderSide::Value side,
                                              bpt::messages::OrderType::Value order_type,
                                              bpt::messages::TimeInForce::Value tif,
                                              int64_t price,
                                              uint64_t quantity,
                                              const std::string& exchange_symbol) = 0;

    virtual void send_cancel(uint64_t order_id,
                             bpt::messages::ExchangeId::Value exchange_id,
                             uint64_t instrument_id) = 0;

    virtual void send_cancel_all(bpt::messages::ExchangeId::Value exchange_id,
                                 uint64_t instrument_id) = 0;

    virtual void send_modify(uint64_t order_id,
                             bpt::messages::ExchangeId::Value exchange_id,
                             uint64_t instrument_id,
                             int64_t new_price,
                             uint64_t new_quantity) = 0;

    virtual void send_account_snapshot_request(bpt::messages::ExchangeId::Value exchange_id,
                                                uint64_t correlation_id) = 0;

    // Drain any pending inbound fragments (exec reports / heartbeats /
    // account snapshots for the Aeron impl; fill-engine ticks for the
    // paper impl). Returns the number of events dispatched.
    virtual int poll(int fragment_limit = 10) = 0;

    // Monotonic wall-clock ns of the most recent gateway heartbeat —
    // consumed by StrategyApp's liveness gate. Paper impls can return
    // the current time (or any value that keeps the liveness check
    // happy) since there's no real gateway to monitor.
    [[nodiscard]] virtual uint64_t last_heartbeat_ns() const = 0;

    // Callbacks are shared data — set by StrategyApp once, fired by
    // whichever concrete impl actually received / generated the event.
    OnExecReportFn on_exec_report;
    OnHeartbeatFn on_heartbeat;
    OnAccountSnapshotFn on_account_snapshot;
};

}  // namespace bpt::strategy::order
