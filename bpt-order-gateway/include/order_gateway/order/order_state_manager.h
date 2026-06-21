#pragma once

#include "order_gateway/messaging/publishers/api/exec_report_publisher.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace bpt::order_gateway::order {

enum class OrderLifecycle : uint8_t {
    PENDING,  // Sent to exchange, awaiting ack
    ACKED,    // Exchange confirmed order is live
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED,
    REJECTED,
};

struct OrderState {
    uint64_t order_id;
    uint64_t exchange_order_id{0};
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id;
    // Exchange-native symbol (e.g. "BTCUSDT" for Binance) — populated from the
    // NewOrder message and used for cancel/modify operations without Sindri.
    std::string exchange_symbol;
    bpt::messages::OrderSide::Value side;
    bpt::messages::OrderType::Value order_type;
    int64_t price;
    uint64_t quantity;
    uint64_t filled_qty{0};
    uint64_t remaining_qty;
    OrderLifecycle lifecycle{OrderLifecycle::PENDING};
    uint64_t created_ns;
    uint64_t last_update_ns;

    /// \brief Build the synthetic CANCELLED exec-report for a stale order
    /// (timed out with no exchange confirmation). `ts` stamps both clocks.
    [[nodiscard]] messaging::api::ExecReport to_cancel_report(uint64_t ts) const {
        return messaging::api::ExecReport{
            .order_id = order_id,
            .exchange_order_id = exchange_order_id,
            .exchange_id = exchange_id,
            .instrument_id = instrument_id,
            .status = bpt::messages::ExecStatus::CANCELLED,
            .side = side,
            .order_type = order_type,
            .price = price,
            .filled_qty = filled_qty,
            .remaining_qty = remaining_qty,
            .reject_reason = bpt::messages::RejectReason::OK,
            .fee = 0,
            .fee_currency = "USDT",
            .exchange_ts_ns = ts,
            .local_ts_ns = ts,
        };
    }
};

// Single-writer order state manager — designed for the hot path thread only.
// All methods must be called from the same thread. No locking.
class OrderStateManager {
public:
    OrderStateManager() { orders_.reserve(512); }

    // Insert a new order in PENDING state. Returns false if order_id already
    // exists.
    bool insert(const OrderState& state);

    // Retrieve an order. Returns nullptr if not found.
    [[nodiscard]] const OrderState* get(uint64_t order_id) const;
    [[nodiscard]] OrderState* get_mut(uint64_t order_id);

    // Update lifecycle. Returns false if order not found.
    bool update(uint64_t order_id,
                OrderLifecycle new_lifecycle,
                uint64_t exchange_order_id,
                uint64_t filled_qty,
                uint64_t remaining_qty,
                uint64_t timestamp_ns);

    // Remove a terminal order (FILLED, CANCELLED, REJECTED).
    void remove(uint64_t order_id);

    // Count open orders for a given exchange (PENDING + ACKED +
    // PARTIALLY_FILLED).
    [[nodiscard]] uint32_t open_order_count(bpt::messages::ExchangeId::Value exchange) const;

    // Total open orders across all venues — O(1).
    [[nodiscard]] uint32_t total_open_orders() const { return total_open_; }

    // Returns all open orders for kill-switch processing.
    template <typename Fn>
    void for_each_open(Fn&& fn) {
        for (auto& [id, state] : orders_) {
            if (state.lifecycle == OrderLifecycle::PENDING || state.lifecycle == OrderLifecycle::ACKED ||
                state.lifecycle == OrderLifecycle::PARTIALLY_FILLED) {
                fn(state);
            }
        }
    }

    // Detect stale orders: ACKED but no update within timeout_ns.
    template <typename Fn>
    void check_stale(uint64_t now_ns, uint64_t timeout_ns, Fn&& fn) {
        for (auto& [id, state] : orders_) {
            if (state.lifecycle == OrderLifecycle::ACKED && now_ns - state.last_update_ns > timeout_ns) {
                fn(state);
            }
        }
    }

private:
    std::unordered_map<uint64_t, OrderState> orders_;
    uint32_t total_open_{0};  // PENDING + ACKED + PARTIALLY_FILLED
};

}  // namespace bpt::order_gateway::order
