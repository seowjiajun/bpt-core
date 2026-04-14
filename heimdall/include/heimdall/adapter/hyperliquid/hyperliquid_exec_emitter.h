#pragma once

// Parses Hyperliquid `/exchange` action responses (the JSON shape HL
// returns from both REST and the WS post path is identical) and emits
// the corresponding ExecEvents onto the adapter's exec queue.
//
// This class owns ONLY the JSON → ExecEvent transformation — it does
// not sign, send, or maintain any client ↔ exchange oid mapping.
// Callers pass that state in via the OrderContext + callbacks on a
// per-call basis.

#include "heimdall/adapter/common/i_order_adapter.h"
#include "heimdall/util/exec_event_queue.h"

#include <bifrost_protocol/OrderSide.h>
#include <bifrost_protocol/OrderType.h>
#include <cstdint>
#include <functional>
#include <string>

namespace heimdall::adapter::hyperliquid {

// All the fields an order ExecEvent needs that can't be recovered from
// HL's response alone (side, type, client-side price/qty, etc.).
// The adapter populates this from the NewOrder SBE message before
// handing both to the emitter.
struct OrderContext {
    uint64_t client_order_id;
    uint64_t instrument_id;
    bifrost::protocol::OrderSide::Value side;
    bifrost::protocol::OrderType::Value order_type;
    int64_t  price_e8;
    uint64_t quantity_e8;
};

class HyperliquidExecEmitter {
public:
    explicit HyperliquidExecEmitter(util::ExecEventQueue& queue) : queue_(queue) {}

    // Parses an order-action response and emits one of:
    //   - ACKED    with exch_oid (from the "resting" status)
    //   - FILLED   with filled_qty + exch_oid (from the "filled" status)
    //   - REJECTED (from "error" statuses or a non-ok top-level status)
    //
    // On a successful ACK, `on_acked(exch_oid)` fires so the caller can
    // record the client_order_id → exch_oid mapping for a future cancel.
    //
    // Returns true if something was emitted, false if the response was
    // missing fields we couldn't recover from. On parse exception, emits
    // a defensive REJECTED so fenrir never waits forever.
    bool emit_order_response(const std::string& resp,
                             const OrderContext& ctx,
                             const std::function<void(uint64_t exch_oid)>& on_acked);

    // Parses a cancel response and emits CANCELLED iff HL reports the
    // cancel as successful. `on_cancelled()` fires so the caller can
    // erase the client→exch oid mapping. Returns true on emit.
    bool emit_cancel_response(const std::string& resp,
                              uint64_t client_order_id,
                              const std::function<void()>& on_cancelled);

    // Fallback path: the adapter caught a transport-level exception and
    // just needs fenrir to see something so its state machine advances.
    void emit_rejected(const OrderContext& ctx);

private:
    util::ExecEventQueue& queue_;
};

}  // namespace heimdall::adapter::hyperliquid
