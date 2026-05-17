#pragma once

/// @file
/// Port: ExecutionReport subscriber. Dispatches decoded fills and order
/// lifecycle events. Aeron concrete in `aeron/exec_subscriber.h`.

#include "bridge/ws/message_encoder.h"

#include <cstdint>
#include <functional>

namespace bpt::bridge::messaging::api {

class ExecSubscriber {
public:
    struct Fill {
        uint64_t ts_ns;
        uint64_t order_id;
        uint64_t instrument_id;
        encode::Side side;
        uint8_t order_type;  ///< raw OrderType enum (0=MARKET, 1=LIMIT)
        double qty;
        double price;
        double fee;
    };

    /// Full order lifecycle event — includes all exec report statuses.
    struct OrderEvent {
        uint64_t ts_ns;
        uint64_t order_id;
        uint64_t instrument_id;
        encode::Side side;
        uint8_t status;      ///< ExecStatus raw value
        uint8_t order_type;  ///< OrderType raw value
        double price;
        double qty;
        double filled_qty;
        double remaining_qty;
    };

    using FillHandler = std::function<void(const Fill&)>;
    using OrderHandler = std::function<void(const OrderEvent&)>;

    virtual ~ExecSubscriber() = default;

    void set_handler(FillHandler h) { handler_ = std::move(h); }
    void set_order_handler(OrderHandler h) { order_handler_ = std::move(h); }

    virtual int poll(int fragment_limit = 32) = 0;

protected:
    FillHandler handler_;
    OrderHandler order_handler_;
};

}  // namespace bpt::bridge::messaging::api
