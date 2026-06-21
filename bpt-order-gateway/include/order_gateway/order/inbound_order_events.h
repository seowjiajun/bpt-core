#pragma once

#include "order_gateway/messaging/publishers/api/exec_report_publisher.h"
#include "order_gateway/order/order_state_manager.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>
#include <messages/TimeInForce.h>

#include <cstdint>
#include <string>

namespace bpt::order_gateway::order {

struct NewOrderEvent {
    uint64_t order_id{0};
    uint64_t instrument_id{0};
    bpt::messages::ExchangeId::Value exchange_id;
    bpt::messages::OrderSide::Value side;
    bpt::messages::OrderType::Value order_type;
    bpt::messages::TimeInForce::Value tif;
    int64_t price{0};
    uint64_t quantity{0};
    uint8_t exec_inst{0};
    std::string exchange_symbol;

    [[nodiscard]] OrderState to_order_state(uint64_t ts) const {
        return OrderState{
            .order_id = order_id,
            .exchange_id = exchange_id,
            .instrument_id = instrument_id,
            .exchange_symbol = exchange_symbol,
            .side = side,
            .order_type = order_type,
            .price = price,
            .quantity = quantity,
            .remaining_qty = quantity,
            .created_ns = ts,
            .last_update_ns = ts,
        };
    }

    [[nodiscard]] messaging::api::ExecReport to_reject_report(bpt::messages::RejectReason::Value reason,
                                                              uint64_t ts) const {
        return messaging::api::ExecReport{
            .order_id = order_id,
            .exchange_order_id = 0,
            .exchange_id = exchange_id,
            .instrument_id = instrument_id,
            .status = bpt::messages::ExecStatus::REJECTED,
            .side = side,
            .order_type = order_type,
            .price = price,
            .filled_qty = 0,
            .remaining_qty = quantity,
            .reject_reason = reason,
            .fee = 0,
            .fee_currency = "USDT",
            .exchange_ts_ns = ts,
            .local_ts_ns = ts,
        };
    }
};

struct CancelOrderEvent {
    uint64_t order_id{0};
    bpt::messages::ExchangeId::Value exchange_id;
};

struct ModifyOrderEvent {
    uint64_t order_id{0};
    bpt::messages::ExchangeId::Value exchange_id;
    int64_t new_price{0};
    uint64_t new_quantity{0};
};

struct CancelAllEvent {
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id{0};
};

}  // namespace bpt::order_gateway::order
