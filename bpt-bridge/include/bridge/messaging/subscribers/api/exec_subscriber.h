#pragma once

/// @file
/// Port: ExecutionReport subscriber. CRTP-templated concrete in
/// `aeron::ExecSubscriber<H>` dispatches on_exec_order_event for all
/// statuses and on_exec_fill only for real fills.

#include "bridge/ws/message_encoder.h"

#include <cstdint>

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

    virtual ~ExecSubscriber() = default;

    virtual int poll(int fragment_limit = 32) = 0;
};

}  // namespace bpt::bridge::messaging::api
