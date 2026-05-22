#pragma once

#include <messages/ExchangeId.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>
#include <messages/exec_inst.h>

#include <cstdint>

namespace bpt::strategy::order {

// Strategy-side exec_inst — readable struct mirroring the uint8 bitmask
// that lands on the wire. Only fields actually wired in messages/exec_inst.h.
struct ExecInst {
    bool post_only = false;

    [[nodiscard]] uint8_t to_bitmask() const {
        return post_only ? bpt::messages::kExecInstPostOnly : 0;
    }
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

}  // namespace bpt::strategy::order
