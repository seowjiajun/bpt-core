#pragma once

#include <messages/ExchangeId.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>
#include <messages/exec_inst.h>

#include <cstdint>
#include <string>

namespace bpt::strategy::order {

// Strategy-side exec_inst — readable struct mirroring the uint8 bitmask
// that lands on the wire. Only fields actually wired in messages/exec_inst.h.
struct ExecInst {
    bool post_only = false;

    [[nodiscard]] uint8_t to_bitmask() const { return post_only ? bpt::messages::kExecInstPostOnly : 0; }
};

struct NewOrderRequest {
    uint64_t instrument_id;
    bpt::messages::ExchangeId::Value exchange_id;
    bpt::messages::OrderSide::Value side;
    bpt::messages::OrderType::Value type;
    bpt::messages::TimeInForce::Value tif;
    double price;
    double qty;
    ExecInst exec_inst{};
};

struct CancelOrderRequest {
    uint64_t order_id;
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id;
};

// Wire-format modify (price/qty already fixed-point 1e8). Distinct from
// NewOrderRequest's natural units — modify has no normalisation step.
struct ModifyOrderRequest {
    uint64_t order_id;
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id;
    int64_t new_price;      // fixed-point 1e8
    uint64_t new_quantity;  // fixed-point 1e8
};

// Wire-ready new order handed across the IOrderGatewayClient boundary:
// order_id assigned, price/qty fixed-point, exec_inst as a bitmask, and
// exchange_symbol resolved. OrderManager builds this from a (natural-units)
// NewOrderRequest; the gateway concrete unpacks it once at SBE encode.
struct OutboundNewOrder {
    uint64_t order_id;
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id;
    bpt::messages::OrderSide::Value side;
    bpt::messages::OrderType::Value order_type;
    bpt::messages::TimeInForce::Value tif;
    int64_t price;      // fixed-point 1e8
    uint64_t quantity;  // fixed-point 1e8
    uint8_t exec_inst;  // bitmask (see ExecInst::to_bitmask)
    std::string exchange_symbol;
};

}  // namespace bpt::strategy::order
