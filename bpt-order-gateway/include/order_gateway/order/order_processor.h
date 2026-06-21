#pragma once

#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/messaging/publishers/aeron/exec_report_publisher.h"
#include "order_gateway/metrics/metrics.h"
#include "order_gateway/order/inbound_order_events.h"
#include "order_gateway/order/order_state_manager.h"
#include "order_gateway/risk/pre_trade_risk_gate.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace bpt::order_gateway::order {

// OrderProcessor is the single point of coordination for all order lifecycle
// events on the hot-path thread.  It owns the logic connecting the four core
// components — ExecReportPublisher, OrderStateManager, RiskChecker, and
// OrderGatewayMetrics — so that main.cpp is purely wiring and polling.
//
// Threading: all public methods must be called from the same thread.  The
// underlying components (OrderStateManager, RiskChecker) are not thread-safe
// by design; the Aeron poll loop provides the single-writer guarantee.
//
// Lifetime: OrderProcessor holds non-owning references to all dependencies.
// The caller (main) is responsible for ensuring they outlive this object.
class OrderProcessor {
public:
    // All dependencies are non-owning references.  The adapters vector is also
    // referenced rather than copied so that the processor always sees the live
    // set without needing to be rebuilt if an adapter is added at runtime.
    OrderProcessor(messaging::api::ExecReportPublisher& exec_pub,
                   OrderStateManager& state_mgr,
                   risk::PreTradeRiskGate& risk_gate,
                   metrics::OrderGatewayMetrics& metrics,
                   const std::vector<std::shared_ptr<adapter::IOrderAdapter>>& adapters);

    // Called by each adapter's on_exec_event callback when an exchange event
    // arrives (ack, partial fill, fill, cancel, reject).
    //
    // Flow:
    //   1. Map ExecStatus → OrderLifecycle.
    //   2. Update state manager with new lifecycle + fill quantities.
    //   3. Release the open-order risk slot for terminal states.
    //   4. Forward the raw exec report to Strategy via ExecReportPublisher.
    //   5. Increment exec_report metrics counter.
    //   6. On ACKED: record order ACK round-trip time (local_ts − created_ns).
    //   7. On terminal: remove order from state manager.
    void on_exec_event(const adapter::ExecEvent& event);

    // Called when Strategy sends a NewOrder on stream 3001.
    //
    // Flow:
    //   1. Risk check (size, notional, open-order count, rate limit).
    //      → Reject immediately with REJECTED exec report if check fails.
    //   2. Adapter lookup by exchange ID.
    //      → Reject with EXCHANGE_ERROR if adapter is absent or disconnected.
    //         The risk slot incremented in step 1 is released here to keep
    //         counters consistent.
    //   3. Insert order into state manager (PENDING lifecycle).
    //   4. Dispatch to adapter's send_new_order.
    void on_new_order(const NewOrderEvent& event);

    // Called when Strategy sends a CancelOrder on stream 3001.
    // Looks up the exchange-native symbol from state (populated at NewOrder
    // time) so the adapter does not need to maintain its own symbol mapping.
    void on_cancel(const CancelOrderEvent& event);

    // Called when Strategy sends a CancelAll on stream 3001.
    //
    // Two modes:
    //   - ExchangeId::ALL  — kill switch: disables trading via RiskChecker,
    //     then iterates all open orders and cancels each one individually.
    //   - Specific exchange — cancels all orders on that venue for the given
    //     instrument_id (0 = all instruments).
    void on_cancel_all(const CancelAllEvent& event);

    // Called when Strategy sends a ModifyOrder on stream 3001.
    // Looks up the exchange-native symbol from state, same as on_cancel.
    void on_modify(const ModifyOrderEvent& event);

    // Scans for orders stuck in ACKED state for longer than stale_timeout_ns.
    // For each stale order:
    //   - Logs a warning with the order ID, exchange, and age.
    //   - Releases the open-order risk slot.
    //   - Publishes a synthetic CANCELLED exec report to Strategy so it can
    //     reconcile its own position and order book.
    //   - Removes the order from state.
    //
    // Called every poll iteration from main; the overhead is proportional to
    // the number of open orders, which is bounded by RiskChecker limits.
    void check_stale_orders(uint64_t stale_timeout_ns);

private:
    // Linear scan over adapters_ — the list is short (≤4 exchanges) so this
    // is faster than a hash map in practice.
    [[nodiscard]] adapter::IOrderAdapter* find_adapter(bpt::messages::ExchangeId::Value id) const;

    // Maps the wire-format ExecStatus onto the internal OrderLifecycle enum.
    // Centralised here so adapters can use the raw protocol type without
    // knowing about OrderLifecycle.
    static OrderLifecycle exec_status_to_lifecycle(bpt::messages::ExecStatus::Value status);

    // String labels used for Prometheus metric tags — must be stable literals.
    static const char* lifecycle_str(OrderLifecycle lc);
    static const char* exchange_str(bpt::messages::ExchangeId::Value id);

    messaging::api::ExecReportPublisher& exec_pub_;
    OrderStateManager& state_mgr_;
    risk::PreTradeRiskGate& risk_gate_;
    metrics::OrderGatewayMetrics& metrics_;
    // O(1) adapter lookup by ExchangeId value (0=ALL unused, 1=BINANCE, …, 4=DERIBIT).
    std::array<adapter::IOrderAdapter*, 5> adapter_by_id_{};
    // Pre-allocated scratch buffer for check_stale_orders to avoid hot-path heap alloc.
    std::vector<uint64_t> stale_ids_scratch_;
};

}  // namespace bpt::order_gateway::order
