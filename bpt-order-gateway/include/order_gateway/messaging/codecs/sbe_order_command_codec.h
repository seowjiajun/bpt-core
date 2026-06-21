#pragma once

#include "order_gateway/order/inbound_order_events.h"

#include <cstddef>
#include <span>

namespace bpt::order_gateway::messaging {

/// Decode-only codec for inbound order commands from strategy.
/// Gateway receives these — it never encodes them.
class SbeOrderCommandCodec {
public:
    order::NewOrderEvent decode_new_order(std::span<const std::byte>);
    order::CancelOrderEvent decode_cancel(std::span<const std::byte>);
    order::ModifyOrderEvent decode_modify(std::span<const std::byte>);
    order::CancelAllEvent decode_cancel_all(std::span<const std::byte>);
};

}  // namespace bpt::order_gateway::messaging
