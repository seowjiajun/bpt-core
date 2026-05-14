#pragma once

// Abstract base for the "order gateway" the strategy talks to.
//
//   AeronOrderGatewayClient — production path; publishes NewOrder /
//     CancelOrder / ModifyOrder messages to bpt-order-gateway over
//     Aeron and consumes the exec-report / heartbeat / account-snapshot
//     streams it publishes back.
//
// The interface is kept narrow on purpose — everything the strategy
// actually does with the gateway (send, cancel, modify, poll, snapshot
// request) is here; Aeron details live only in the Aeron impl.
//
// Historical note: a PaperOrderGatewayClient used to sit alongside
// Aeron as an in-process synthetic exchange driven by a PaperFillEngine.
// Removed 2026-04-22 after a session showed it gave systematically
// optimistic (misleading) fill behaviour; see
// `feedback_avoid_synthetic_fills.md` for the rationale and why future
// "paper" capabilities — if ever needed — should live as peer venue
// adapters rather than modal mutations of the strategy path.

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
                                              uint8_t exec_inst,
                                              const std::string& exchange_symbol) = 0;

    virtual void send_cancel(uint64_t order_id,
                             bpt::messages::ExchangeId::Value exchange_id,
                             uint64_t instrument_id) = 0;

    virtual void send_cancel_all(bpt::messages::ExchangeId::Value exchange_id, uint64_t instrument_id) = 0;

    virtual void send_modify(uint64_t order_id,
                             bpt::messages::ExchangeId::Value exchange_id,
                             uint64_t instrument_id,
                             int64_t new_price,
                             uint64_t new_quantity) = 0;

    virtual void send_account_snapshot_request(bpt::messages::ExchangeId::Value exchange_id,
                                               uint64_t correlation_id) = 0;

    // Drain any pending inbound Aeron fragments (exec reports,
    // heartbeats, account snapshots). Returns the number of events
    // dispatched.
    virtual int poll(int fragment_limit = 10) = 0;

    // Monotonic wall-clock ns of the most recent gateway heartbeat —
    // consumed by StrategyService's liveness gate.
    [[nodiscard]] virtual uint64_t last_heartbeat_ns() const = 0;

    // Callbacks are shared data — set by StrategyService once, fired by
    // whichever concrete impl actually received / generated the event.
    OnExecReportFn on_exec_report;
    OnHeartbeatFn on_heartbeat;
    OnAccountSnapshotFn on_account_snapshot;
};

}  // namespace bpt::strategy::order
