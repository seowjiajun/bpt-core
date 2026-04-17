#pragma once

// Deribit WebSocket action JSON-RPC builders — pure transformations
// from SBE protocol values to the `{"jsonrpc":"2.0",...}` shape
// Deribit's WS API expects. No state, no I/O. Each helper takes the
// req_id explicitly so the adapter's monotonic counter stays in one
// place.
//
// Deribit's order API lives entirely over WebSocket — there is no
// REST fallback — so the codec produces the final serialised string
// for the caller to write directly to the socket.

#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace bpt::order_gateway::adapter::deribit {

struct OrderSpec {
    std::string                            instrument_name;  // e.g. "BTC-PERPETUAL"
    bpt::messages::OrderSide::Value    side;
    bpt::messages::OrderType::Value    order_type;
    bpt::messages::TimeInForce::Value  tif;
    int64_t                                price_e8;         // natural * 1e8
    uint64_t                               quantity_e8;      // natural * 1e8
    std::string                            label;            // client-side label (cloid)
};

// `public/auth` with grant_type=client_credentials.
[[nodiscard]] std::string build_auth_msg(std::string_view client_id,
                                          std::string_view client_secret,
                                          uint64_t req_id);

// `private/buy` or `private/sell` depending on side. Limit orders
// carry price + time_in_force; market orders omit both.
[[nodiscard]] std::string build_new_order_msg(const OrderSpec& spec, uint64_t req_id);

// `private/cancel` by exchange order id (Deribit doesn't cancel by
// client label — caller must look up the exchange id via exec_parser).
[[nodiscard]] std::string build_cancel_msg(std::string_view exchange_order_id, uint64_t req_id);

// `private/edit` — Deribit's native amend. Takes new price + amount.
[[nodiscard]] std::string build_edit_msg(std::string_view exchange_order_id,
                                          int64_t new_price_e8,
                                          uint64_t new_quantity_e8,
                                          uint64_t req_id);

// `public/test` — reply to Deribit's heartbeat test_request.
[[nodiscard]] std::string build_test_response(uint64_t req_id);

// Simple JSON-RPC wrapper: `{"jsonrpc":"2.0","id":N,"method":M,"params":{}}`.
// Used by the adapter's post-login bring-up sequence
// (enable_cancel_on_disconnect, set_heartbeat, private/subscribe).
[[nodiscard]] std::string build_simple_rpc(std::string_view method,
                                            const std::string& params_json,
                                            uint64_t req_id);

}  // namespace bpt::order_gateway::adapter::deribit
