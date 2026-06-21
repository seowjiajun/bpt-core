#pragma once

#include <messages/ExchangeId.h>
#include <messages/OrderSide.h>

#include <cstdint>

namespace bpt::strategy::order {

// Lifecycle record owned by OrderManager. Strategies hold OrderHandles
// pointing at these; OM updates them on ExecutionReport.
struct OrderState {
    enum class Status : uint8_t {
        Live,             // resting on the book
        PartiallyFilled,  // resting with partial fills accumulated
        CancelPending,    // cancel sent, ack not yet received
        Filled,           // terminal: fully filled
        Cancelled,        // terminal
        Rejected,         // terminal
    };

    uint64_t order_id{0};
    uint64_t instrument_id{0};
    bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
    bpt::messages::OrderSide::Value side{bpt::messages::OrderSide::NULL_VALUE};
    double price{0.0};           // last submitted price
    double qty{0.0};             // original order size
    double filled_qty{0.0};      // running cumulative
    double avg_fill_price{0.0};  // running VWAP of fills
    Status status{Status::Live};
    uint64_t created_ns{0};
    uint64_t last_update_ns{0};
    uint8_t tag{0};  // strategy-defined intent (e.g. Quote vs Unwind)

    bool live() const { return status == Status::Live || status == Status::PartiallyFilled; }
    bool terminal() const {
        return status == Status::Filled || status == Status::Cancelled || status == Status::Rejected;
    }
    bool cancel_pending() const { return status == Status::CancelPending; }
};

// Pointer-into-OM-storage. Stable for the process lifetime — OM uses
// std::deque so push_back never invalidates existing pointers, and
// terminal records are kept until pruned (Phase 3).
struct OrderHandle {
    OrderState* state{nullptr};

    bool valid() const { return state != nullptr; }
    bool live() const { return state && state->live(); }
    bool terminal() const { return state && state->terminal(); }
    bool cancel_pending() const { return state && state->cancel_pending(); }

    // Convenience: 0 if !valid (matches the old order_id == 0 idiom).
    uint64_t order_id() const { return state ? state->order_id : 0; }

    void reset() { state = nullptr; }
};

}  // namespace bpt::strategy::order
