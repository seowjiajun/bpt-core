#pragma once

// PaperFillEngine — pure-logic synthetic exchange used in canary /
// shadow runs. Accepts order submissions + cancels, tracks a per-
// (exchange, instrument) BBO + resting-order book, and emits fill /
// reject / cancel events driven by market-data flow.
//
// Scope (MVP):
//   - LIMIT + GTC / IOC / FOK / POST_ONLY
//   - No MARKET (rejected — out-of-scope until someone needs it)
//   - Instant cross-at-BBO for IOC/FOK
//   - Trade-print sweep for resting GTC (fill when a print touches or
//     crosses our limit — optimistic "no queue ahead of us" assumption)
//   - No partial fills (every fill consumes the full order quantity)
//   - No fees (strategies can apply their own via FeeCache if needed)
//
// What this deliberately skips for now: queue-position modelling,
// order-book depth simulation, cancel/replace race conditions. Good
// enough to catch "does the new strategy version still post sane
// quotes and avoid obvious inventory blowups?" — the point of Tier 2
// canary. Richer simulation can replace it later without touching
// callers, since the engine is behind PaperOrderGatewayClient.
//
// Not thread-safe — runs on the same thread as the strategy poll loop.

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>
#include <messages/TimeInForce.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::order {

// Plain-data event emitted by the engine and consumed by
// PaperOrderGatewayClient, which converts to an SBE ExecutionReport
// before firing the strategy's on_exec_report callback.
struct PaperFillEvent {
    uint64_t order_id{0};
    bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
    uint64_t instrument_id{0};
    bpt::messages::ExecStatus::Value status{bpt::messages::ExecStatus::NULL_VALUE};
    bpt::messages::OrderSide::Value side{bpt::messages::OrderSide::NULL_VALUE};
    bpt::messages::OrderType::Value order_type{bpt::messages::OrderType::NULL_VALUE};
    int64_t price_e8{0};          // fill price for FILLED; original order price otherwise
    uint64_t filled_qty_e8{0};
    uint64_t remaining_qty_e8{0};
    bpt::messages::RejectReason::Value reject_reason{bpt::messages::RejectReason::NULL_VALUE};
    uint64_t ts_ns{0};
};

class PaperFillEngine {
public:
    struct Order {
        uint64_t order_id{0};
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
        uint64_t instrument_id{0};
        bpt::messages::OrderSide::Value side{bpt::messages::OrderSide::NULL_VALUE};
        bpt::messages::OrderType::Value order_type{bpt::messages::OrderType::NULL_VALUE};
        bpt::messages::TimeInForce::Value tif{bpt::messages::TimeInForce::NULL_VALUE};
        int64_t price_e8{0};
        uint64_t quantity_e8{0};
    };

    // Submit an order. Per-tif handling:
    //   IOC / FOK LIMIT — filled immediately if crosses current BBO,
    //                     else REJECTED (LIMIT_ORDER_NOT_CROSSING).
    //                     FOK additionally requires full-qty cross.
    //   POST_ONLY LIMIT — REJECTED (POST_ONLY_VIOLATION) if it would
    //                     cross at placement; otherwise ACKED + rests.
    //   GTC LIMIT — ACKED + rests; waits for trade print to sweep.
    //   MARKET — REJECTED (RISK_REJECTED) — out of scope for MVP.
    void submit(const Order& o, uint64_t submit_ns);

    // Cancel a known resting order. Emits CANCELLED. No-op (not an
    // error) if order_id isn't in the book — it may have already
    // filled or been cancelled.
    void cancel(uint64_t order_id,
                bpt::messages::ExchangeId::Value exchange_id,
                uint64_t instrument_id,
                uint64_t cancel_ns);

    // Cancel every resting order for (exchange, instrument). Emits
    // CANCELLED for each.
    void cancel_all(bpt::messages::ExchangeId::Value exchange_id,
                    uint64_t instrument_id,
                    uint64_t cancel_ns);

    // Feed current BBO. Keyed by instrument_id — our refdata ids are
    // globally unique across venues so we don't need a separate
    // exchange dimension here (the MD SBE messages don't carry one).
    // Only updates internal state used by IOC/FOK cross-check on the
    // next submit(); does NOT fill resting orders (those require a
    // trade print so we know a real match occurred).
    void on_bbo(uint64_t instrument_id, double bid, double ask, uint64_t ts_ns);

    // Feed a trade print. Any resting order whose limit price is swept
    // by the print fills at its limit price (optimistic maker-fill,
    // no queue modelling).
    void on_trade(uint64_t instrument_id, double price, double qty, uint64_t ts_ns);

    // Drain up to `limit` pending events into the callback. Returns
    // the number of events dispatched.
    int drain(int limit, const std::function<void(const PaperFillEvent&)>& cb);

    // Introspection for tests + metrics.
    [[nodiscard]] std::size_t resting_count() const;
    [[nodiscard]] std::size_t pending_count() const { return pending_.size(); }

private:
    struct Bbo {
        double bid{0.0};
        double ask{0.0};
        bool valid{false};
    };

    void emit_reject(const Order& o,
                     bpt::messages::RejectReason::Value reason,
                     uint64_t ts_ns);
    void emit_ack(const Order& o, uint64_t ts_ns);
    void emit_fill(const Order& o, int64_t fill_price_e8, uint64_t ts_ns);
    void emit_cancel(const Order& o, uint64_t ts_ns);

    std::unordered_map<uint64_t, Bbo> bbo_;                       // instrument_id → BBO
    std::unordered_map<uint64_t, std::vector<Order>> resting_;    // instrument_id → orders
    std::deque<PaperFillEvent> pending_;
};

}  // namespace bpt::strategy::order
