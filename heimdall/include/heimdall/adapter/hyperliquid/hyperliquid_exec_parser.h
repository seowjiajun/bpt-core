#pragma once

#include "heimdall/adapter/common/i_order_adapter.h"

#include <boost/json.hpp>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace heimdall::adapter {

// Parses Hyperliquid WebSocket fill events into ExecEvents.
//
// HL doesn't send a cloid back in userFills, and each userFills entry is a
// single partial slice — not "the whole fill for this order." So we need to
// track per-order state across multiple fill events: when the cumulative
// filled quantity finally reaches the original order size, we emit FILLED;
// before that, every slice is PARTIAL.
//
// Fenrir's strategies treat FILLED as a terminal status and erase their
// per-order routing state. If we emit FILLED on the first partial slice of a
// multi-slice fill, every subsequent slice gets dropped — which is exactly
// the bug that was causing fenrir's inventory to diverge from the exchange.
class HyperliquidExecParser {
public:
    std::function<void(const ExecEvent&)> on_exec_event;

    // Register an order the moment we know it's resting on HL (or filled on
    // placement). The parser needs the original quantity to decide when a
    // multi-slice fill is complete.
    //
    // Called from the order adapter's on_acked callback — see
    // hyperliquid_order_adapter.cpp. Re-registering the same exch_oid is a
    // no-op (HL never reuses oids).
    void register_order(uint64_t exch_oid, uint64_t client_order_id, uint64_t original_qty_e8);

    // Called when channel=="userFills" data.fills arrives.
    void handle_fills(const boost::json::array& fills, uint64_t recv_ns);

private:
    struct PendingOrder {
        uint64_t client_order_id;
        uint64_t original_qty_e8;
        uint64_t cumulative_filled_e8;
    };

    std::unordered_map<uint64_t, PendingOrder> pending_;
};

}  // namespace heimdall::adapter
