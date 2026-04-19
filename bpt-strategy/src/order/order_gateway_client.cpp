#include "strategy/order/order_gateway_client.h"

#include <messages/AccountSnapshot.h>
#include <messages/AccountSnapshotRequest.h>
#include <messages/CancelAll.h>
#include <messages/CancelOrder.h>
#include <messages/ExecutionReport.h>
#include <messages/MessageHeader.h>
#include <messages/ModifyOrder.h>
#include <messages/NewOrder.h>
#include <messages/OrderGatewayHeartbeat.h>

#include <x86intrin.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::strategy::order {

OrderGatewayClient::OrderGatewayClient(std::shared_ptr<aeron::Aeron> aeron,
                                       const std::string& channel,
                                       int order_stream,
                                       int exec_report_stream,
                                       int heartbeat_stream,
                                       int account_snapshot_stream) {
    order_pub_ = bpt::common::aeron::wait_for_publication(aeron, channel, order_stream);
    exec_report_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, exec_report_stream);
    heartbeat_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, heartbeat_stream);

    if (account_snapshot_stream != 0) {
        account_snapshot_sub_ =
            bpt::common::aeron::wait_for_subscription(aeron, channel, account_snapshot_stream);
        account_snapshot_assembler_ = std::make_unique<aeron::FragmentAssembler>(
            [this](aeron::AtomicBuffer& buf,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& hdr) { handle_account_snapshot_fragment(buf, offset, length, hdr); });
    }

    exec_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t offset, aeron::util::index_t length, aeron::Header& hdr) {
            handle_exec_report_fragment(buf, offset, length, hdr);
        });

    hb_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t offset, aeron::util::index_t length, aeron::Header& hdr) {
            handle_heartbeat_fragment(buf, offset, length, hdr);
        });
}

bool OrderGatewayClient::send_new_order(uint64_t order_id,
                                        bpt::messages::ExchangeId::Value exchange_id,
                                        uint64_t instrument_id,
                                        bpt::messages::OrderSide::Value side,
                                        bpt::messages::OrderType::Value order_type,
                                        bpt::messages::TimeInForce::Value tif,
                                        int64_t price,
                                        uint64_t quantity,
                                        const std::string& exchange_symbol) {
    using namespace bpt::messages;

    if (quantity == 0) {
        bpt::common::log::warn("[OrderGW] Rejected order_id={}: quantity is zero", order_id);
        return false;
    }
    if (order_type != OrderType::MARKET && price <= 0) {
        bpt::common::log::warn("[OrderGW] Rejected order_id={}: price={} invalid for non-MARKET order", order_id, price);
        return false;
    }
    if (exchange_symbol.empty()) {
        bpt::common::log::warn("[OrderGW] Rejected order_id={}: exchange_symbol is empty", order_id);
        return false;
    }

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + NewOrder::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    NewOrder msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .orderId(order_id)
        .exchangeId(exchange_id)
        .instrumentId(instrument_id)
        .side(side)
        .orderType(order_type)
        .timeInForce(tif)
        .price(price)
        .quantity(quantity)
        .timestampNs(bpt::common::util::TscClock::now_epoch_ns())
        .putExchangeSymbol(exchange_symbol);

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<aeron::util::index_t>(kBufSize));
    while (order_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(kBufSize)) < 0)
        _mm_pause();
    return true;
}

void OrderGatewayClient::send_cancel(uint64_t order_id,
                                     bpt::messages::ExchangeId::Value exchange_id,
                                     uint64_t instrument_id) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + CancelOrder::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    CancelOrder msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .orderId(order_id)
        .exchangeId(exchange_id)
        .instrumentId(instrument_id)
        .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<aeron::util::index_t>(kBufSize));
    while (order_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(kBufSize)) < 0)
        _mm_pause();
}

void OrderGatewayClient::send_cancel_all(bpt::messages::ExchangeId::Value exchange_id, uint64_t instrument_id) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + CancelAll::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    CancelAll msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(exchange_id)
        .instrumentId(instrument_id)
        .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<aeron::util::index_t>(kBufSize));
    while (order_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(kBufSize)) < 0)
        _mm_pause();
}

void OrderGatewayClient::send_modify(uint64_t order_id,
                                     bpt::messages::ExchangeId::Value exchange_id,
                                     uint64_t instrument_id,
                                     int64_t new_price,
                                     uint64_t new_quantity) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + ModifyOrder::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    ModifyOrder msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .orderId(order_id)
        .exchangeId(exchange_id)
        .instrumentId(instrument_id)
        .newPrice(new_price)
        .newQuantity(new_quantity)
        .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<aeron::util::index_t>(kBufSize));
    while (order_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(kBufSize)) < 0)
        _mm_pause();
}

void OrderGatewayClient::send_account_snapshot_request(bpt::messages::ExchangeId::Value exchange_id,
                                                       uint64_t correlation_id) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + AccountSnapshotRequest::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    AccountSnapshotRequest msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(exchange_id)
        .correlationId(correlation_id)
        .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<aeron::util::index_t>(kBufSize));
    while (order_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(kBufSize)) < 0)
        _mm_pause();
}

int OrderGatewayClient::poll(int fragment_limit) {
    int total = 0;
    total += exec_report_sub_->poll(exec_assembler_->handler(), fragment_limit);
    total += heartbeat_sub_->poll(hb_assembler_->handler(), fragment_limit);
    if (account_snapshot_sub_)
        total += account_snapshot_sub_->poll(account_snapshot_assembler_->handler(), fragment_limit);
    return total;
}

void OrderGatewayClient::handle_exec_report_fragment(aeron::AtomicBuffer& buf,
                                                     aeron::util::index_t offset,
                                                     aeron::util::index_t length,
                                                     aeron::Header& /*hdr*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buf.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() != ExecutionReport::sbeTemplateId())
        return;

    ExecutionReport msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<std::size_t>(length));

    if (on_exec_report)
        on_exec_report(msg);
}

void OrderGatewayClient::handle_heartbeat_fragment(aeron::AtomicBuffer& buf,
                                                   aeron::util::index_t offset,
                                                   aeron::util::index_t length,
                                                   aeron::Header& /*hdr*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buf.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() != OrderGatewayHeartbeat::sbeTemplateId())
        return;

    OrderGatewayHeartbeat msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<std::size_t>(length));

    last_heartbeat_ns_ = msg.timestampNs();

    if (on_heartbeat)
        on_heartbeat(msg);
}

void OrderGatewayClient::handle_account_snapshot_fragment(aeron::AtomicBuffer& buf,
                                                          aeron::util::index_t offset,
                                                          aeron::util::index_t length,
                                                          aeron::Header& /*hdr*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buf.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() != AccountSnapshot::sbeTemplateId())
        return;

    AccountSnapshot msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<std::size_t>(length));

    if (on_account_snapshot)
        on_account_snapshot(msg);  // non-const: SBE group iterators require mutation
}

}  // namespace bpt::strategy::order
