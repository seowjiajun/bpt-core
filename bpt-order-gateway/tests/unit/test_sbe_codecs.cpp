// SBE encode/decode round-trip tests for OrderGateway order gateway messages.
// Tests messages 10–15: NewOrder, CancelOrder, CancelAll, ModifyOrder,
// ExecutionReport, OrderGatewayHeartbeat. No Aeron, no network.

#include <messages/CancelAll.h>
#include <messages/CancelOrder.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/ExecutionReport.h>
#include <messages/MessageHeader.h>
#include <messages/ModifyOrder.h>
#include <messages/NewOrder.h>
#include <messages/OrderGatewayHeartbeat.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>
#include <messages/TimeInForce.h>
#include <messages/exec_inst.h>

#include <cstring>
#include <gtest/gtest.h>

namespace {

using namespace bpt::messages;

// ── NewOrder (id=10) ─────────────────────────────────────────────────────────

TEST(NewOrderTest, TemplateId) {
    EXPECT_EQ(NewOrder::sbeTemplateId(), 10u);
    EXPECT_EQ(NewOrder::sbeSchemaId(), 1u);
}

TEST(NewOrderTest, EncodeDecodeAllFields) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + NewOrder::sbeBlockLength();
    char buf[kSz]{};

    NewOrder msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .orderId(999ULL)
        .exchangeId(ExchangeId::BINANCE)
        .instrumentId(42ULL)
        .side(OrderSide::BUY)
        .orderType(OrderType::LIMIT)
        .timeInForce(TimeInForce::GTC)
        .price(3000000000000LL)  // $30000 * 1e8
        .quantity(100000000ULL)  // 1.0 * 1e8
        .timestampNs(123456789ULL)
        .putExchangeSymbol("BTCUSDT");

    MessageHeader hdr(buf, kSz);
    EXPECT_EQ(hdr.templateId(), NewOrder::sbeTemplateId());

    NewOrder dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.orderId(), 999ULL);
    EXPECT_EQ(dec.exchangeId(), ExchangeId::BINANCE);
    EXPECT_EQ(dec.instrumentId(), 42ULL);
    EXPECT_EQ(dec.side(), OrderSide::BUY);
    EXPECT_EQ(dec.orderType(), OrderType::LIMIT);
    EXPECT_EQ(dec.timeInForce(), TimeInForce::GTC);
    EXPECT_EQ(dec.price(), 3000000000000LL);
    EXPECT_EQ(dec.quantity(), 100000000ULL);
    EXPECT_EQ(dec.timestampNs(), 123456789ULL);
    EXPECT_EQ(dec.getExchangeSymbolAsString(), "BTCUSDT");
}

TEST(NewOrderTest, MarketOrderFields) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + NewOrder::sbeBlockLength();
    char buf[kSz]{};

    NewOrder msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .orderId(1ULL)
        .exchangeId(ExchangeId::OKX)
        .instrumentId(7ULL)
        .side(OrderSide::SELL)
        .orderType(OrderType::MARKET)
        .timeInForce(TimeInForce::IOC)
        .price(0LL)
        .quantity(50000000ULL)
        .timestampNs(0ULL)
        .putExchangeSymbol("BTC-USDT");

    MessageHeader hdr(buf, kSz);
    NewOrder dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.orderId(), 1ULL);
    EXPECT_EQ(dec.exchangeId(), ExchangeId::OKX);
    EXPECT_EQ(dec.side(), OrderSide::SELL);
    EXPECT_EQ(dec.orderType(), OrderType::MARKET);
    EXPECT_EQ(dec.timeInForce(), TimeInForce::IOC);
    EXPECT_EQ(dec.price(), 0LL);
    EXPECT_EQ(dec.getExchangeSymbolAsString(), "BTC-USDT");
}

TEST(NewOrderTest, PostOnlyWithFOK) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + NewOrder::sbeBlockLength();
    char buf[kSz]{};

    NewOrder msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .orderId(55ULL)
        .exchangeId(ExchangeId::HYPERLIQUID)
        .instrumentId(3ULL)
        .side(OrderSide::BUY)
        .orderType(OrderType::LIMIT)
        .timeInForce(TimeInForce::FOK)
        .price(10000000000LL)
        .quantity(200000000ULL)
        .timestampNs(9999ULL)
        .execInst(bpt::messages::kExecInstPostOnly)
        .putExchangeSymbol("BTC");

    MessageHeader hdr(buf, kSz);
    NewOrder dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.orderType(), OrderType::LIMIT);
    EXPECT_EQ(dec.execInst() & bpt::messages::kExecInstPostOnly, bpt::messages::kExecInstPostOnly);
    EXPECT_EQ(dec.timeInForce(), TimeInForce::FOK);
    EXPECT_EQ(dec.exchangeId(), ExchangeId::HYPERLIQUID);
    EXPECT_EQ(dec.getExchangeSymbolAsString(), "BTC");
}

// ── CancelOrder (id=11) ──────────────────────────────────────────────────────

TEST(CancelOrderTest, TemplateId) {
    EXPECT_EQ(CancelOrder::sbeTemplateId(), 11u);
}

TEST(CancelOrderTest, EncodeDecodeAllFields) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + CancelOrder::sbeBlockLength();
    char buf[kSz]{};

    CancelOrder msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .orderId(777ULL)
        .exchangeId(ExchangeId::OKX)
        .instrumentId(99ULL)
        .timestampNs(8888888ULL);

    MessageHeader hdr(buf, kSz);
    CancelOrder dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.orderId(), 777ULL);
    EXPECT_EQ(dec.exchangeId(), ExchangeId::OKX);
    EXPECT_EQ(dec.instrumentId(), 99ULL);
    EXPECT_EQ(dec.timestampNs(), 8888888ULL);
}

// ── CancelAll (id=12) ────────────────────────────────────────────────────────

TEST(CancelAllTest, TemplateId) {
    EXPECT_EQ(CancelAll::sbeTemplateId(), 12u);
}

TEST(CancelAllTest, AllVenues) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + CancelAll::sbeBlockLength();
    char buf[kSz]{};

    CancelAll msg;
    msg.wrapAndApplyHeader(buf, 0, kSz).exchangeId(ExchangeId::ALL).instrumentId(0ULL).timestampNs(111ULL);

    MessageHeader hdr(buf, kSz);
    CancelAll dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.exchangeId(), ExchangeId::ALL);
    EXPECT_EQ(dec.instrumentId(), 0ULL);
    EXPECT_EQ(dec.timestampNs(), 111ULL);
}

TEST(CancelAllTest, SpecificVenueAndInstrument) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + CancelAll::sbeBlockLength();
    char buf[kSz]{};

    CancelAll msg;
    msg.wrapAndApplyHeader(buf, 0, kSz).exchangeId(ExchangeId::BINANCE).instrumentId(42ULL).timestampNs(222ULL);

    MessageHeader hdr(buf, kSz);
    CancelAll dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.exchangeId(), ExchangeId::BINANCE);
    EXPECT_EQ(dec.instrumentId(), 42ULL);
}

// ── ModifyOrder (id=13) ──────────────────────────────────────────────────────

TEST(ModifyOrderTest, TemplateId) {
    EXPECT_EQ(ModifyOrder::sbeTemplateId(), 13u);
}

TEST(ModifyOrderTest, EncodeDecodeAllFields) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + ModifyOrder::sbeBlockLength();
    char buf[kSz]{};

    ModifyOrder msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .orderId(12345ULL)
        .exchangeId(ExchangeId::BINANCE)
        .instrumentId(7ULL)
        .newPrice(4000000000000LL)
        .newQuantity(150000000ULL)
        .timestampNs(333ULL);

    MessageHeader hdr(buf, kSz);
    ModifyOrder dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.orderId(), 12345ULL);
    EXPECT_EQ(dec.exchangeId(), ExchangeId::BINANCE);
    EXPECT_EQ(dec.instrumentId(), 7ULL);
    EXPECT_EQ(dec.newPrice(), 4000000000000LL);
    EXPECT_EQ(dec.newQuantity(), 150000000ULL);
    EXPECT_EQ(dec.timestampNs(), 333ULL);
}

// ── ExecutionReport (id=14) ──────────────────────────────────────────────────

TEST(ExecutionReportTest, TemplateId) {
    EXPECT_EQ(ExecutionReport::sbeTemplateId(), 14u);
}

TEST(ExecutionReportTest, EncodeDecodeFilledReport) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + ExecutionReport::sbeBlockLength();
    char buf[kSz]{};

    ExecutionReport msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .orderId(100ULL)
        .exchangeOrderId(200ULL)
        .exchangeId(ExchangeId::BINANCE)
        .instrumentId(42ULL)
        .status(ExecStatus::FILLED)
        .side(OrderSide::BUY)
        .orderType(OrderType::LIMIT)
        .price(3000000000000LL)
        .filledQty(100000000ULL)
        .remainingQty(0ULL)
        .rejectReason(RejectReason::OK)
        .fee(50000LL)
        .putFeeCurrency([]() {
            static const char b[8] = "USDT";
            return b;
        }())
        .timestampNs(555555ULL)
        .localTsNs(555666ULL);

    MessageHeader hdr(buf, kSz);
    ExecutionReport dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.orderId(), 100ULL);
    EXPECT_EQ(dec.exchangeOrderId(), 200ULL);
    EXPECT_EQ(dec.exchangeId(), ExchangeId::BINANCE);
    EXPECT_EQ(dec.instrumentId(), 42ULL);
    EXPECT_EQ(dec.status(), ExecStatus::FILLED);
    EXPECT_EQ(dec.side(), OrderSide::BUY);
    EXPECT_EQ(dec.orderType(), OrderType::LIMIT);
    EXPECT_EQ(dec.price(), 3000000000000LL);
    EXPECT_EQ(dec.filledQty(), 100000000ULL);
    EXPECT_EQ(dec.remainingQty(), 0ULL);
    EXPECT_EQ(dec.rejectReason(), RejectReason::OK);
    EXPECT_EQ(dec.fee(), 50000LL);
    EXPECT_EQ(dec.getFeeCurrencyAsString(), "USDT");
    EXPECT_EQ(dec.timestampNs(), 555555ULL);
    EXPECT_EQ(dec.localTsNs(), 555666ULL);
}

TEST(ExecutionReportTest, RejectedReport) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + ExecutionReport::sbeBlockLength();
    char buf[kSz]{};

    ExecutionReport msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .orderId(77ULL)
        .exchangeOrderId(0ULL)
        .exchangeId(ExchangeId::OKX)
        .instrumentId(5ULL)
        .status(ExecStatus::REJECTED)
        .side(OrderSide::SELL)
        .orderType(OrderType::MARKET)
        .price(0LL)
        .filledQty(0ULL)
        .remainingQty(50000000ULL)
        .rejectReason(RejectReason::RISK_REJECTED)
        .fee(0LL)
        .putFeeCurrency([]() {
            static const char b[8] = "USDT";
            return b;
        }())
        .timestampNs(1ULL)
        .localTsNs(1ULL);

    MessageHeader hdr(buf, kSz);
    ExecutionReport dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.status(), ExecStatus::REJECTED);
    EXPECT_EQ(dec.rejectReason(), RejectReason::RISK_REJECTED);
    EXPECT_EQ(dec.side(), OrderSide::SELL);
    EXPECT_EQ(dec.orderType(), OrderType::MARKET);
}

TEST(ExecutionReportTest, PartialFillReport) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + ExecutionReport::sbeBlockLength();
    char buf[kSz]{};

    ExecutionReport msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .orderId(88ULL)
        .exchangeOrderId(999ULL)
        .exchangeId(ExchangeId::HYPERLIQUID)
        .instrumentId(10ULL)
        .status(ExecStatus::PARTIAL)
        .side(OrderSide::BUY)
        .orderType(OrderType::LIMIT)
        .price(2500000000000LL)
        .filledQty(30000000ULL)
        .remainingQty(70000000ULL)
        .rejectReason(RejectReason::OK)
        .fee(-10000LL)  // negative fee = rebate
        .putFeeCurrency([]() {
            static const char b[8] = "BTC";
            return b;
        }())
        .timestampNs(12345ULL)
        .localTsNs(12400ULL);

    MessageHeader hdr(buf, kSz);
    ExecutionReport dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.status(), ExecStatus::PARTIAL);
    EXPECT_EQ(dec.filledQty(), 30000000ULL);
    EXPECT_EQ(dec.remainingQty(), 70000000ULL);
    EXPECT_EQ(dec.fee(), -10000LL);
    EXPECT_EQ(dec.getFeeCurrencyAsString(), "BTC");
    EXPECT_EQ(dec.exchangeId(), ExchangeId::HYPERLIQUID);
}

TEST(ExecutionReportTest, AllRejectReasonValues) {
    EXPECT_EQ(static_cast<uint8_t>(RejectReason::OK), 0u);
    EXPECT_EQ(static_cast<uint8_t>(RejectReason::INVALID_PRICE), 1u);
    EXPECT_EQ(static_cast<uint8_t>(RejectReason::INVALID_QTY), 2u);
    EXPECT_EQ(static_cast<uint8_t>(RejectReason::INSUFFICIENT_BALANCE), 3u);
    EXPECT_EQ(static_cast<uint8_t>(RejectReason::RATE_LIMITED), 4u);
    EXPECT_EQ(static_cast<uint8_t>(RejectReason::EXCHANGE_ERROR), 5u);
    EXPECT_EQ(static_cast<uint8_t>(RejectReason::RISK_REJECTED), 6u);
    EXPECT_EQ(static_cast<uint8_t>(RejectReason::DUPLICATE_ORDER_ID), 7u);
}

TEST(ExecutionReportTest, AllExecStatusValues) {
    EXPECT_EQ(static_cast<uint8_t>(ExecStatus::ACKED), 0u);
    EXPECT_EQ(static_cast<uint8_t>(ExecStatus::FILLED), 1u);
    EXPECT_EQ(static_cast<uint8_t>(ExecStatus::PARTIAL), 2u);
    EXPECT_EQ(static_cast<uint8_t>(ExecStatus::REJECTED), 3u);
    EXPECT_EQ(static_cast<uint8_t>(ExecStatus::CANCELLED), 4u);
}

// ── OrderGatewayHeartbeat (id=15) ─────────────────────────────────────────────────

TEST(OrderGatewayHeartbeatTest, TemplateId) {
    EXPECT_EQ(OrderGatewayHeartbeat::sbeTemplateId(), 15u);
}

TEST(OrderGatewayHeartbeatTest, EncodeDecodeAllFields) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + OrderGatewayHeartbeat::sbeBlockLength();
    char buf[kSz]{};

    OrderGatewayHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .serviceId(1u)
        .timestampNs(987654321ULL)
        .ordersInFlight(42u)
        .exchangeStatus(0x03u);  // BINANCE + OKX connected

    MessageHeader hdr(buf, kSz);
    OrderGatewayHeartbeat dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.serviceId(), 1u);
    EXPECT_EQ(dec.timestampNs(), 987654321ULL);
    EXPECT_EQ(dec.ordersInFlight(), 42u);
    EXPECT_EQ(dec.exchangeStatus(), 0x03u);
}

TEST(OrderGatewayHeartbeatTest, AllDisconnected) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + OrderGatewayHeartbeat::sbeBlockLength();
    char buf[kSz]{};

    OrderGatewayHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kSz).serviceId(1u).timestampNs(1ULL).ordersInFlight(0u).exchangeStatus(0x00u);

    MessageHeader hdr(buf, kSz);
    OrderGatewayHeartbeat dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.ordersInFlight(), 0u);
    EXPECT_EQ(dec.exchangeStatus(), 0x00u);
}

TEST(OrderGatewayHeartbeatTest, AllExchangesConnected) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + OrderGatewayHeartbeat::sbeBlockLength();
    char buf[kSz]{};

    // Bitmask: bit0=BINANCE, bit1=OKX, bit2=HYPERLIQUID → 0x07
    OrderGatewayHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .serviceId(1u)
        .timestampNs(1000000ULL)
        .ordersInFlight(100u)
        .exchangeStatus(0x07u);

    MessageHeader hdr(buf, kSz);
    OrderGatewayHeartbeat dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.exchangeStatus(), 0x07u);
    EXPECT_EQ(dec.ordersInFlight(), 100u);
}

TEST(OrderGatewayHeartbeatTest, MaxOrdersInFlight) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + OrderGatewayHeartbeat::sbeBlockLength();
    char buf[kSz]{};

    OrderGatewayHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .serviceId(1u)
        .timestampNs(0ULL)
        .ordersInFlight(0xFFFFu)  // max uint16
        .exchangeStatus(0u);

    MessageHeader hdr(buf, kSz);
    OrderGatewayHeartbeat dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.ordersInFlight(), 0xFFFFu);
}

// ── Exchange ID enum values ───────────────────────────────────────────────────

TEST(ExchangeIdTest, Values) {
    EXPECT_EQ(static_cast<uint8_t>(ExchangeId::ALL), 0u);
    EXPECT_EQ(static_cast<uint8_t>(ExchangeId::BINANCE), 1u);
    EXPECT_EQ(static_cast<uint8_t>(ExchangeId::OKX), 2u);
    EXPECT_EQ(static_cast<uint8_t>(ExchangeId::HYPERLIQUID), 3u);
}

TEST(OrderSideTest, Values) {
    EXPECT_EQ(static_cast<uint8_t>(OrderSide::BUY), 0u);
    EXPECT_EQ(static_cast<uint8_t>(OrderSide::SELL), 1u);
}

TEST(OrderTypeTest, Values) {
    EXPECT_EQ(static_cast<uint8_t>(OrderType::MARKET), 0u);
    EXPECT_EQ(static_cast<uint8_t>(OrderType::LIMIT), 1u);
}

TEST(ExecInstTest, Values) {
    EXPECT_EQ(bpt::messages::kExecInstPostOnly, 0x01u);
}

TEST(TimeInForceTest, Values) {
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::GTC), 0u);
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::IOC), 1u);
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::FOK), 2u);
}

// Removed FeeCurrencyTest enum-value test — FeeCurrency was promoted from
// uint8 enum to Char8 string field; there are no integer values to assert.
// Round-trip coverage of the new string field is exercised by
// ExecutionReportTest above via wrap-encode-decode.

}  // namespace
