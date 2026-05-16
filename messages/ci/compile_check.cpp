// Compile-check and round-trip test for all bpt-messages generated headers.
//
// Verifies:
//   1. Every generated header compiles cleanly under C++17.
//   2. Static schema metadata is accessible and correct.
//   3. Encode → decode round-trips produce identical values for all message
//      types (the most common source of silent regression).
//
// Run by the CI pipeline on every push to main, and by the CMake CTest target
// bpt_messages_check when building bpt-messages standalone.

#include "messages/AckStatus.h"
#include "messages/BacktestAck.h"
#include "messages/BacktestCommand.h"
#include "messages/BacktestControl.h"
#include "messages/DeltaUpdateType.h"
#include "messages/GroupSizeEncoding.h"
#include "messages/InstrumentStatus.h"
#include "messages/InstrumentType.h"
#include "messages/MdMarketData.h"
#include "messages/MdServiceHeartbeat.h"
#include "messages/MdSubscribeBatch.h"
#include "messages/MdSubscriptionAck.h"
#include "messages/MdSubscriptionHeartbeat.h"
#include "messages/MdTrade.h"
#include "messages/MessageHeader.h"
#include "messages/OptionSide.h"
#include "messages/RefDataDelta.h"
#include "messages/RefDataSnapshot.h"
#include "messages/RefDataSubscriptionRequest.h"
#include "messages/TradeSide.h"

#include <cassert>
#include <cstring>
#include <string>

using bpt::messages::AckStatus;
using bpt::messages::BacktestAck;
using bpt::messages::BacktestCommand;
using bpt::messages::BacktestControl;
using bpt::messages::DeltaUpdateType;
using bpt::messages::InstrumentStatus;
using bpt::messages::InstrumentType;
using bpt::messages::MdMarketData;
using bpt::messages::MdServiceHeartbeat;
using bpt::messages::MdSubscribeBatch;
using bpt::messages::MdSubscriptionAck;
using bpt::messages::MdSubscriptionHeartbeat;
using bpt::messages::MdTrade;
using bpt::messages::MessageHeader;
using bpt::messages::OptionSide;
using bpt::messages::RefDataDelta;
using bpt::messages::RefDataSnapshot;
using bpt::messages::RefDataSubscriptionRequest;
using bpt::messages::TradeSide;

// Verify static metadata for all message types.
static_assert(RefDataSubscriptionRequest::SBE_TEMPLATE_ID == 1);
static_assert(RefDataSnapshot::SBE_TEMPLATE_ID == 2);
static_assert(RefDataDelta::SBE_TEMPLATE_ID == 3);
static_assert(MdSubscribeBatch::SBE_TEMPLATE_ID == 4);
static_assert(MdSubscriptionAck::SBE_TEMPLATE_ID == 5);
static_assert(MdSubscriptionHeartbeat::SBE_TEMPLATE_ID == 6);
static_assert(MdMarketData::SBE_TEMPLATE_ID == 7);
static_assert(MdTrade::SBE_TEMPLATE_ID == 8);
static_assert(MdServiceHeartbeat::SBE_TEMPLATE_ID == 9);
static_assert(BacktestAck::SBE_TEMPLATE_ID == 24);
static_assert(BacktestControl::SBE_TEMPLATE_ID == 25);
static_assert(RefDataSubscriptionRequest::SBE_SCHEMA_ID == 1);
static_assert(RefDataSnapshot::SBE_SCHEMA_ID == 1);
static_assert(RefDataDelta::SBE_SCHEMA_ID == 1);
static_assert(MdSubscribeBatch::SBE_SCHEMA_ID == 1);
static_assert(MdSubscriptionAck::SBE_SCHEMA_ID == 1);
static_assert(MdSubscriptionHeartbeat::SBE_SCHEMA_ID == 1);
static_assert(MdMarketData::SBE_SCHEMA_ID == 1);
static_assert(MdTrade::SBE_SCHEMA_ID == 1);
static_assert(MdServiceHeartbeat::SBE_SCHEMA_ID == 1);

// ── helpers ─────────────────────────────────────────────────────────────────

static std::string trim_null(const char* s, std::size_t maxlen) {
    std::size_t n = strnlen(s, maxlen);
    return std::string(s, n);
}

// ── RefDataSubscriptionRequest round-trip ───────────────────────────────────

static void test_subscription_request() {
    char buf[512]{};

    // Encode
    RefDataSubscriptionRequest req;
    req.wrapAndApplyHeader(buf, 0, sizeof(buf)).correlationId(42).timestampNs(1234567890ULL);

    auto& instruments = req.instrumentsCount(2);
    instruments.next().putSymbol("BTC-USDT-PERP").putExchange("BINANCE");
    instruments.next().putSymbol("ETH-USDT-PERP").putExchange("OKX");

    // Decode
    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == RefDataSubscriptionRequest::sbeTemplateId());

    RefDataSubscriptionRequest dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.correlationId() == 42);
    assert(dec.timestampNs() == 1234567890ULL);

    auto& g = dec.instruments();
    assert(g.hasNext());
    g.next();
    assert(trim_null(g.symbol(), 24) == "BTC-USDT-PERP");
    assert(trim_null(g.exchange(), 8) == "BINANCE");
    assert(g.hasNext());
    g.next();
    assert(trim_null(g.symbol(), 24) == "ETH-USDT-PERP");
    assert(trim_null(g.exchange(), 8) == "OKX");
    assert(!g.hasNext());
}

// ── RefDataSnapshot round-trip ───────────────────────────────────────────────

static void test_snapshot() {
    char buf[1024]{};

    // Encode
    RefDataSnapshot snap;
    snap.wrapAndApplyHeader(buf, 0, sizeof(buf)).correlationId(99).snapshotSeqNum(200).timestampNs(9876543210ULL);

    auto& instruments = snap.instrumentsCount(1);
    instruments.next()
        .instrumentId(777)
        .putSymbol("BTC-USDT-PERP")
        .putExchange("BINANCE")
        .putBaseCurrency("BTC")
        .putQuoteCurrency("USDT")
        .instrumentType(InstrumentType::PERPETUAL)
        .status(InstrumentStatus::ACTIVE)
        .lotSize(0.001)
        .tickSize(0.1)
        .contractSize(1.0)
        .expiryDate(0)
        .optionSide(OptionSide::NA)
        .strikePrice(0.0);

    // Decode
    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == RefDataSnapshot::sbeTemplateId());

    RefDataSnapshot dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.correlationId() == 99);
    assert(dec.snapshotSeqNum() == 200);
    assert(dec.timestampNs() == 9876543210ULL);

    auto& g = dec.instruments();
    assert(g.hasNext());
    g.next();
    assert(g.instrumentId() == 777);
    assert(trim_null(g.symbol(), 24) == "BTC-USDT-PERP");
    assert(trim_null(g.exchange(), 8) == "BINANCE");
    assert(trim_null(g.baseCurrency(), 8) == "BTC");
    assert(trim_null(g.quoteCurrency(), 8) == "USDT");
    assert(g.instrumentType() == InstrumentType::PERPETUAL);
    assert(g.status() == InstrumentStatus::ACTIVE);
    assert(g.lotSize() == 0.001);
    assert(g.tickSize() == 0.1);
    assert(g.contractSize() == 1.0);
    assert(g.expiryDate() == 0);
    assert(g.optionSide() == OptionSide::NA);
    assert(g.strikePrice() == 0.0);
    assert(!g.hasNext());
}

// ── RefDataDelta round-trip ──────────────────────────────────────────────────

static void test_delta() {
    char buf[512]{};

    // Encode
    RefDataDelta delta;
    delta.wrapAndApplyHeader(buf, 0, sizeof(buf))
        .deltaSeqNum(101)
        .timestampNs(1234567891ULL)
        .updateType(DeltaUpdateType::MODIFY)
        .instrumentId(42)
        .putSymbol("BTC-USDT-PERP")
        .putExchange("BINANCE")
        .putBaseCurrency("BTC")
        .putQuoteCurrency("USDT")
        .instrumentType(InstrumentType::PERPETUAL)
        .status(InstrumentStatus::HALTED)
        .lotSize(0.001)
        .tickSize(0.1)
        .contractSize(1.0)
        .expiryDate(0)
        .optionSide(OptionSide::NA)
        .strikePrice(0.0);

    // Decode
    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == RefDataDelta::sbeTemplateId());

    RefDataDelta dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.deltaSeqNum() == 101);
    assert(dec.timestampNs() == 1234567891ULL);
    assert(dec.updateType() == DeltaUpdateType::MODIFY);
    assert(dec.instrumentId() == 42);
    assert(trim_null(dec.symbol(), 24) == "BTC-USDT-PERP");
    assert(trim_null(dec.exchange(), 8) == "BINANCE");
    assert(trim_null(dec.baseCurrency(), 8) == "BTC");
    assert(trim_null(dec.quoteCurrency(), 8) == "USDT");
    assert(dec.instrumentType() == InstrumentType::PERPETUAL);
    assert(dec.status() == InstrumentStatus::HALTED);
    assert(dec.lotSize() == 0.001);
    assert(dec.tickSize() == 0.1);
    assert(dec.contractSize() == 1.0);
    assert(dec.expiryDate() == 0);
    assert(dec.optionSide() == OptionSide::NA);
    assert(dec.strikePrice() == 0.0);
}

// ── Heartbeat convention: NULL_VALUE update type ─────────────────────────────

static void test_heartbeat() {
    char buf[512]{};

    RefDataDelta hb;
    hb.wrapAndApplyHeader(buf, 0, sizeof(buf))
        .deltaSeqNum(5)
        .timestampNs(111ULL)
        .updateType(DeltaUpdateType::NULL_VALUE)
        .instrumentId(0);

    MessageHeader hdr(buf, sizeof(buf));
    RefDataDelta dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.deltaSeqNum() == 5);
    assert(dec.updateType() == DeltaUpdateType::NULL_VALUE);
    assert(dec.instrumentId() == 0);
}

// ── MdSubscribeBatch round-trip ──────────────────────────────────────────────

static void test_md_subscribe_batch() {
    char buf[512]{};

    MdSubscribeBatch msg;
    msg.wrapAndApplyHeader(buf, 0, sizeof(buf)).correlationId(1001ULL).timestampNs(5555ULL);

    auto& g = msg.instrumentsCount(2);
    g.next().instrumentId(100ULL).putExchange("BINANCE").putSymbol("BTCUSDT");
    g.next().instrumentId(200ULL).putExchange("OKX").putSymbol("BTC-USDT-SWAP");

    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == MdSubscribeBatch::sbeTemplateId());

    MdSubscribeBatch dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.correlationId() == 1001ULL);
    assert(dec.timestampNs() == 5555ULL);

    auto& dg = dec.instruments();
    assert(dg.hasNext());
    dg.next();
    assert(dg.instrumentId() == 100ULL);
    assert(trim_null(dg.exchange(), 8) == "BINANCE");
    assert(trim_null(dg.symbol(), 24) == "BTCUSDT");
    assert(dg.hasNext());
    dg.next();
    assert(dg.instrumentId() == 200ULL);
    assert(trim_null(dg.exchange(), 8) == "OKX");
    assert(trim_null(dg.symbol(), 24) == "BTC-USDT-SWAP");
    assert(!dg.hasNext());
}

// ── MdSubscriptionAck round-trip ─────────────────────────────────────────────

static void test_md_subscription_ack() {
    char buf[256]{};

    MdSubscriptionAck msg;
    msg.wrapAndApplyHeader(buf, 0, sizeof(buf))
        .correlationId(1001ULL)
        .timestampNs(6666ULL)
        .instrumentId(100ULL)
        .putExchange("BINANCE")
        .ackStatus(AckStatus::OK);

    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == MdSubscriptionAck::sbeTemplateId());

    MdSubscriptionAck dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.correlationId() == 1001ULL);
    assert(dec.timestampNs() == 6666ULL);
    assert(dec.instrumentId() == 100ULL);
    assert(trim_null(dec.exchange(), 8) == "BINANCE");
    assert(dec.ackStatus() == AckStatus::OK);
}

// ── MdSubscriptionHeartbeat round-trip ───────────────────────────────────────

static void test_md_subscription_heartbeat() {
    char buf[256]{};

    MdSubscriptionHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, sizeof(buf)).timestampNs(7777ULL).instrumentId(100ULL).seqNum(42ULL);

    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == MdSubscriptionHeartbeat::sbeTemplateId());

    MdSubscriptionHeartbeat dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.timestampNs() == 7777ULL);
    assert(dec.instrumentId() == 100ULL);
    assert(dec.seqNum() == 42ULL);
}

// ── MdMarketData round-trip ───────────────────────────────────────────────────

static void test_md_market_data() {
    char buf[256]{};

    MdMarketData msg;
    msg.wrapAndApplyHeader(buf, 0, sizeof(buf))
        .timestampNs(8888ULL)
        .instrumentId(100ULL)
        .bidPrice(29990.5)
        .bidQty(1.25)
        .askPrice(29991.0)
        .askQty(0.75)
        .seqNum(999ULL);

    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == MdMarketData::sbeTemplateId());

    MdMarketData dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.timestampNs() == 8888ULL);
    assert(dec.instrumentId() == 100ULL);
    assert(dec.bidPrice() == 29990.5);
    assert(dec.bidQty() == 1.25);
    assert(dec.askPrice() == 29991.0);
    assert(dec.askQty() == 0.75);
    assert(dec.seqNum() == 999ULL);
}

// ── MdTrade round-trip ────────────────────────────────────────────────────────

static void test_md_trade() {
    char buf[256]{};

    MdTrade msg;
    msg.wrapAndApplyHeader(buf, 0, sizeof(buf))
        .timestampNs(9999ULL)
        .instrumentId(100ULL)
        .price(30000.0)
        .qty(0.5)
        .side(TradeSide::BUY)
        .seqNum(1000ULL);

    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == MdTrade::sbeTemplateId());

    MdTrade dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.timestampNs() == 9999ULL);
    assert(dec.instrumentId() == 100ULL);
    assert(dec.price() == 30000.0);
    assert(dec.qty() == 0.5);
    assert(dec.side() == TradeSide::BUY);
    assert(dec.seqNum() == 1000ULL);
}

// ── MdServiceHeartbeat round-trip ─────────────────────────────────────────────

static void test_md_service_heartbeat() {
    char buf[128]{};

    MdServiceHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, sizeof(buf)).timestampNs(11111ULL).seqNum(55ULL);

    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == MdServiceHeartbeat::sbeTemplateId());

    MdServiceHeartbeat dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.timestampNs() == 11111ULL);
    assert(dec.seqNum() == 55ULL);
}

// ── BacktestAck round-trip ────────────────────────────────────────────────────

static void test_backtest_ack() {
    char buf[128]{};

    BacktestAck msg;
    msg.wrapAndApplyHeader(buf, 0, sizeof(buf)).tickSeqNum(42ULL).simulationTs(1700000000000000000ULL);

    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == BacktestAck::sbeTemplateId());

    BacktestAck dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.tickSeqNum() == 42ULL);
    assert(dec.simulationTs() == 1700000000000000000ULL);
}

// ── BacktestControl round-trip ────────────────────────────────────────────────

static void test_backtest_control() {
    char buf[128]{};

    BacktestControl msg;
    msg.wrapAndApplyHeader(buf, 0, sizeof(buf))
        .command(BacktestCommand::START)
        .tickSeqNum(1ULL)
        .simulationTs(1700000000000000000ULL);

    MessageHeader hdr(buf, sizeof(buf));
    assert(hdr.templateId() == BacktestControl::sbeTemplateId());

    BacktestControl dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), sizeof(buf));

    assert(dec.command() == BacktestCommand::START);
    assert(dec.tickSeqNum() == 1ULL);
    assert(dec.simulationTs() == 1700000000000000000ULL);
}

int main() {
    test_subscription_request();
    test_snapshot();
    test_delta();
    test_heartbeat();
    test_md_subscribe_batch();
    test_md_subscription_ack();
    test_md_subscription_heartbeat();
    test_md_market_data();
    test_md_trade();
    test_md_service_heartbeat();
    test_backtest_ack();
    test_backtest_control();
    return 0;
}
