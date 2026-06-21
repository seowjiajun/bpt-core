#include "order_gateway/messaging/codecs/sbe_order_command_codec.h"

#include <messages/CancelAll.h>
#include <messages/CancelOrder.h>
#include <messages/MessageHeader.h>
#include <messages/ModifyOrder.h>
#include <messages/NewOrder.h>

namespace bpt::order_gateway::messaging {

using bpt::messages::CancelAll;
using bpt::messages::CancelOrder;
using bpt::messages::MessageHeader;
using bpt::messages::ModifyOrder;
using bpt::messages::NewOrder;

order::NewOrderEvent SbeOrderCommandCodec::decode_new_order(std::span<const std::byte> bytes) {
    auto* data = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
    const auto len = bytes.size();

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), len);

    NewOrder msg;
    msg.wrapForDecode(data, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), len);

    return order::NewOrderEvent{
        .order_id = msg.orderId(),
        .instrument_id = msg.instrumentId(),
        .exchange_id = msg.exchangeId(),
        .side = msg.side(),
        .order_type = msg.orderType(),
        .tif = msg.timeInForce(),
        .price = msg.price(),
        .quantity = msg.quantity(),
        .exec_inst = msg.execInst(),
        .exchange_symbol = msg.getExchangeSymbolAsString(),
    };
}

order::CancelOrderEvent SbeOrderCommandCodec::decode_cancel(std::span<const std::byte> bytes) {
    auto* data = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
    const auto len = bytes.size();

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), len);

    CancelOrder msg;
    msg.wrapForDecode(data, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), len);

    return order::CancelOrderEvent{
        .order_id = msg.orderId(),
        .exchange_id = msg.exchangeId(),
    };
}

order::ModifyOrderEvent SbeOrderCommandCodec::decode_modify(std::span<const std::byte> bytes) {
    auto* data = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
    const auto len = bytes.size();

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), len);

    ModifyOrder msg;
    msg.wrapForDecode(data, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), len);

    return order::ModifyOrderEvent{
        .order_id = msg.orderId(),
        .exchange_id = msg.exchangeId(),
        .new_price = msg.newPrice(),
        .new_quantity = msg.newQuantity(),
    };
}

order::CancelAllEvent SbeOrderCommandCodec::decode_cancel_all(std::span<const std::byte> bytes) {
    auto* data = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
    const auto len = bytes.size();

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), len);

    CancelAll msg;
    msg.wrapForDecode(data, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), len);

    return order::CancelAllEvent{
        .exchange_id = msg.exchangeId(),
        .instrument_id = msg.instrumentId(),
    };
}

}  // namespace bpt::order_gateway::messaging
