#pragma once

#include "bridge/ws/message_encoder.h"

#include <Aeron.h>

#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::bridge {

// Subscribes to OrderGateway's exec report stream and delivers decoded fills
// and order lifecycle events.
class ExecSubscriber {
public:
    struct Fill {
        uint64_t ts_ns;
        uint64_t order_id;
        uint64_t instrument_id;
        encode::Side side;
        uint8_t order_type;  // raw OrderType enum (0=MARKET, 1=LIMIT). The
                             // POST_ONLY constraint lives in NewOrder.execInst,
                             // not here; the fill's MAKER/TAKER role is on liquidity.
        double qty;          // natural units
        double price;        // natural units
        double fee;          // in quote currency (natural units, signed — positive = paid)
    };

    // Full order lifecycle event — includes all exec report statuses.
    struct OrderEvent {
        uint64_t ts_ns;
        uint64_t order_id;
        uint64_t instrument_id;
        encode::Side side;
        uint8_t status;      // ExecStatus raw value
        uint8_t order_type;  // OrderType raw value
        double price;
        double qty;  // original order qty
        double filled_qty;
        double remaining_qty;
    };

    using FillHandler = std::function<void(const Fill&)>;
    using OrderHandler = std::function<void(const OrderEvent&)>;

    ExecSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    void set_handler(FillHandler h) { handler_ = std::move(h); }
    void set_order_handler(OrderHandler h) { order_handler_ = std::move(h); }

    int poll(int fragment_limit = 32);

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& header);

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    FillHandler handler_;
    OrderHandler order_handler_;
};

}  // namespace bpt::bridge
