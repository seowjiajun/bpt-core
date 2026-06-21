#pragma once

/// @file
/// Abstract interface for the "order gateway" the strategy talks to.
/// Implementations:
///
///   AeronOrderGatewayClient<Handler> — production path; publishes
///     NewOrder / CancelOrder / ModifyOrder messages to bpt-order-gateway
///     over Aeron and consumes the exec-report / heartbeat /
///     account-snapshot streams it publishes back. Templated on the
///     Handler that receives parsed inbound events (Handler is
///     `StrategyService` in prod).
///
/// The interface is kept narrow on purpose — only the send + poll surface
/// strategies actually call. Inbound dispatch is via the templated
/// concrete (handler->on_exec_report etc., direct calls, no
/// std::function indirection).
///
/// Historical note: a PaperOrderGatewayClient used to sit alongside
/// Aeron as an in-process synthetic exchange driven by a PaperFillEngine.
/// Removed 2026-04-22 after a session showed it gave systematically
/// optimistic (misleading) fill behaviour; see
/// `feedback_avoid_synthetic_fills.md`.

#include "strategy/order/requests.h"

#include <messages/ExchangeId.h>

#include <cstdint>

namespace bpt::strategy::order {

class IOrderGatewayClient {
public:
    virtual ~IOrderGatewayClient() = default;

    // Send a new order. See AeronOrderGatewayClient for validation rules
    // (non-zero quantity, positive price for LIMIT, non-empty symbol).
    // Returns false if pre-flight checks fail; the caller must undo any
    // bookkeeping tied to the rejected order.
    [[nodiscard]] virtual bool send_new_order(const OutboundNewOrder& order) = 0;

    virtual void send_cancel(const CancelOrderRequest& cancel) = 0;

    virtual void send_cancel_all(bpt::messages::ExchangeId::Value exchange_id, uint64_t instrument_id) = 0;

    virtual void send_modify(const ModifyOrderRequest& modify) = 0;

    virtual void send_account_snapshot_request(bpt::messages::ExchangeId::Value exchange_id,
                                               uint64_t correlation_id) = 0;

    // Drain any pending inbound Aeron fragments (exec reports,
    // heartbeats, account snapshots). Returns the number of events
    // dispatched.
    virtual int poll(int fragment_limit = 10) = 0;

    // Monotonic wall-clock ns of the most recent gateway heartbeat —
    // consumed by StrategyService's liveness gate.
    [[nodiscard]] virtual uint64_t last_heartbeat_ns() const = 0;
};

}  // namespace bpt::strategy::order
