#pragma once

// Binance REST action query-string builders — pure transformations from
// SBE protocol values to the `key=value&key=value` format Binance's
// /api/v3/order endpoint expects. No timestamp, no signature — the auth
// layer appends those right before sending, so these functions stay
// deterministic and are trivially unit-testable.
//
// All price and quantity fields are scaled by 1e8 in fenrir's internal
// representation and divided out here to the decimal strings Binance
// wants on the wire.

#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace bpt::order_gateway::adapter::binance {

struct OrderSpec {
    std::string                            symbol;        // e.g. "BTCUSDT"
    bpt::messages::OrderSide::Value    side;
    bpt::messages::OrderType::Value    order_type;
    bpt::messages::TimeInForce::Value  tif;
    int64_t                                price_e8;      // natural * 1e8
    uint64_t                               quantity_e8;   // natural * 1e8
    std::string                            cloid;
};

// Build the unsigned POST /api/v3/order query params for a new order.
[[nodiscard]] std::string build_new_order_params(const OrderSpec& spec);

// Build the unsigned DELETE /api/v3/order query params for a cancel
// matched by client order id.
[[nodiscard]] std::string build_cancel_params(std::string_view symbol,
                                               std::string_view cloid);

// Build the unsigned POST /api/v3/order query params for the replace
// leg of a cancel+replace modify. Binance has no native amend, so
// send_modify does a cancel followed by this. Note: side isn't in
// ModifyOrder so the caller must supply it; the original implementation
// hardcoded BUY/LIMIT/GTC which was a known bug — left intact here
// pending a higher-layer fix.
[[nodiscard]] std::string build_modify_replace_params(std::string_view symbol,
                                                       std::string_view new_cloid,
                                                       int64_t new_price_e8,
                                                       uint64_t new_quantity_e8);

}  // namespace bpt::order_gateway::adapter::binance
