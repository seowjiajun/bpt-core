#pragma once

/// \file
/// \brief Hyperliquid `/exchange` response → ExecEvent emitter.
///
/// Parses HL's `/exchange` action responses (the JSON shape HL returns
/// from both REST and the WS post path is identical) and emits the
/// corresponding ExecEvents onto the adapter's exec queue.
///
/// This class owns ONLY the JSON → ExecEvent transformation — it does
/// not sign, send, or maintain any client ↔ exchange oid mapping.
/// Callers pass that state in via the OrderContext + callbacks on a
/// per-call basis.

#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/util/exec_event_queue.h"

#include <messages/OrderSide.h>
#include <messages/OrderType.h>

#include <cstdint>
#include <functional>
#include <string>

namespace bpt::order_gateway::adapter::hyperliquid {

/// \brief All the fields an order ExecEvent needs that can't be recovered from HL's response.
///
/// The adapter populates this from the NewOrder SBE message before
/// handing both to the emitter.
struct OrderContext {
    uint64_t client_order_id;
    uint64_t instrument_id;
    bpt::messages::OrderSide::Value side;
    bpt::messages::OrderType::Value order_type;
    int64_t price_e8;
    uint64_t quantity_e8;
};

class HyperliquidExecEmitter {
public:
    explicit HyperliquidExecEmitter(util::ExecEventQueue& queue) : queue_(queue) {}

    /// \brief Parse an order-action response and emit the corresponding ExecEvent.
    ///
    /// Emits one of:
    ///   - ACKED    with exch_oid (from the "resting" status).
    ///   - FILLED   with filled_qty + exch_oid (from the "filled" status).
    ///   - REJECTED (from "error" statuses or a non-ok top-level status).
    ///
    /// On a successful ACK, `on_acked(exch_oid)` fires so the caller
    /// can record the client_order_id → exch_oid mapping for a future
    /// cancel.
    ///
    /// \return True if something was emitted, false if the response was
    ///         missing fields we couldn't recover from. On parse
    ///         exception, emits a defensive REJECTED so callers never
    ///         wait forever.
    bool emit_order_response(const std::string& resp,
                             const OrderContext& ctx,
                             const std::function<void(uint64_t exch_oid)>& on_acked);

    /// \brief Parse a cancel response and emit CANCELLED iff HL reports the cancel as successful.
    ///
    /// `on_cancelled()` fires so the caller can erase the client→exch
    /// oid mapping.
    /// \return True on emit.
    bool emit_cancel_response(const std::string& resp,
                              uint64_t client_order_id,
                              const std::function<void()>& on_cancelled);

    /// \brief Fallback emit when the adapter caught a transport-level exception.
    ///
    /// Just needs the strategy to see something so its state machine
    /// advances.
    void emit_rejected(const OrderContext& ctx);

    /// \name Phantom-fill recovery emits
    /// See hyperliquid_reconciler.h. Called when a post_action failure
    /// was followed by REST `/info` reconciliation that located the
    /// order as either resting (emit_recovered_ack) or already-filled
    /// (emit_recovered_fill). Shape is identical to what
    /// emit_order_response would have produced from the live response.
    /// @{
    void emit_recovered_ack(const OrderContext& ctx, uint64_t exch_oid);
    void emit_recovered_fill(const OrderContext& ctx,
                             uint64_t exch_oid,
                             int64_t fill_price_e8,
                             int64_t fill_fee_e8,
                             uint64_t fill_qty_e8,
                             uint64_t fill_time_ns);
    /// @}

private:
    util::ExecEventQueue& queue_;
};

}  // namespace bpt::order_gateway::adapter::hyperliquid
