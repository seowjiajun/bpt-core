#include <messages/AckStatus.h>
#include <messages/MdMarketData.h>
#include <messages/MdServiceHeartbeat.h>
#include <messages/MdSubscribeBatch.h>
#include <messages/MdSubscriptionAck.h>
#include <messages/MdSubscriptionHeartbeat.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>
#include <messages/TradeSide.h>

#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

using namespace bpt::messages;

// ── MdSubscribeBatch ─────────────────────────────────────────────────────────

TEST(MdSubscribeBatchTest, TemplateId) {
    EXPECT_EQ(MdSubscribeBatch::sbeTemplateId(), 4u);
    EXPECT_EQ(MdSubscribeBatch::sbeSchemaId(), 1u);
}

TEST(MdSubscribeBatchTest, EncodeDecodeHeaderAndGroup) {
    constexpr uint16_t kN = 2;
    std::vector<char> buf(MessageHeader::encodedLength() + MdSubscribeBatch::sbeBlockLength() +
                              MdSubscribeBatch::Instruments::sbeHeaderSize() +
                              kN * MdSubscribeBatch::Instruments::sbeBlockLength(),
                          '\0');

    MdSubscribeBatch msg;
    msg.wrapAndApplyHeader(buf.data(), 0, buf.size()).correlationId(77ULL).timestampNs(12345ULL);

    auto& g = msg.instrumentsCount(kN);
    g.next().instrumentId(100ULL).putExchange("BINANCE").putSymbol("BTCUSDT");
    g.next().instrumentId(200ULL).putExchange("OKX").putSymbol("BTC-USDT-SWAP");

    MessageHeader hdr(buf.data(), buf.size());
    EXPECT_EQ(hdr.templateId(), MdSubscribeBatch::sbeTemplateId());

    MdSubscribeBatch dec;
    dec.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    EXPECT_EQ(dec.correlationId(), 77ULL);
    EXPECT_EQ(dec.timestampNs(), 12345ULL);

    auto& dg = dec.instruments();
    ASSERT_EQ(dg.count(), kN);

    ASSERT_TRUE(dg.hasNext());
    dg.next();
    EXPECT_EQ(dg.instrumentId(), 100ULL);
    EXPECT_EQ(dg.getExchangeAsString(), "BINANCE");
    EXPECT_EQ(dg.getSymbolAsString(), "BTCUSDT");

    ASSERT_TRUE(dg.hasNext());
    dg.next();
    EXPECT_EQ(dg.instrumentId(), 200ULL);
    EXPECT_EQ(dg.getExchangeAsString(), "OKX");
    EXPECT_EQ(dg.getSymbolAsString(), "BTC-USDT-SWAP");

    EXPECT_FALSE(dg.hasNext());
}

TEST(MdSubscribeBatchTest, EmptyGroup) {
    std::vector<char> buf(MessageHeader::encodedLength() + MdSubscribeBatch::sbeBlockLength() +
                              MdSubscribeBatch::Instruments::sbeHeaderSize(),
                          '\0');

    MdSubscribeBatch msg;
    msg.wrapAndApplyHeader(buf.data(), 0, buf.size()).correlationId(1ULL).timestampNs(0ULL);
    msg.instrumentsCount(0);

    MessageHeader hdr(buf.data(), buf.size());
    MdSubscribeBatch dec;
    dec.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    EXPECT_EQ(dec.instruments().count(), 0u);
}

// ── MdSubscriptionAck ────────────────────────────────────────────────────────

TEST(MdSubscriptionAckTest, TemplateId) {
    EXPECT_EQ(MdSubscriptionAck::sbeTemplateId(), 5u);
}

TEST(MdSubscriptionAckTest, EncodeDecodeOk) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + MdSubscriptionAck::sbeBlockLength();
    char buf[kSz]{};

    MdSubscriptionAck msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .correlationId(42ULL)
        .timestampNs(999ULL)
        .instrumentId(100ULL)
        .putExchange("BINANCE")
        .ackStatus(AckStatus::OK);

    MessageHeader hdr(buf, kSz);
    MdSubscriptionAck dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.correlationId(), 42ULL);
    EXPECT_EQ(dec.timestampNs(), 999ULL);
    EXPECT_EQ(dec.instrumentId(), 100ULL);
    EXPECT_EQ(dec.getExchangeAsString(), "BINANCE");
    EXPECT_EQ(dec.ackStatus(), AckStatus::OK);
}

TEST(MdSubscriptionAckTest, AckStatusValues) {
    EXPECT_EQ(static_cast<uint8_t>(AckStatus::OK), 0u);
    EXPECT_EQ(static_cast<uint8_t>(AckStatus::REJECTED), 1u);
    EXPECT_EQ(static_cast<uint8_t>(AckStatus::NOT_FOUND), 2u);
}

TEST(MdSubscriptionAckTest, EncodeDecodeNotFound) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + MdSubscriptionAck::sbeBlockLength();
    char buf[kSz]{};

    MdSubscriptionAck msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .correlationId(1ULL)
        .timestampNs(0ULL)
        .instrumentId(999ULL)
        .putExchange("UNKNOWN")
        .ackStatus(AckStatus::NOT_FOUND);

    MessageHeader hdr(buf, kSz);
    MdSubscriptionAck dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.ackStatus(), AckStatus::NOT_FOUND);
    EXPECT_EQ(dec.instrumentId(), 999ULL);
}

// ── MdSubscriptionHeartbeat ──────────────────────────────────────────────────

TEST(MdSubscriptionHeartbeatTest, TemplateId) {
    EXPECT_EQ(MdSubscriptionHeartbeat::sbeTemplateId(), 6u);
}

TEST(MdSubscriptionHeartbeatTest, EncodeDecodeAllFields) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + MdSubscriptionHeartbeat::sbeBlockLength();
    char buf[kSz]{};

    MdSubscriptionHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kSz).timestampNs(88888ULL).instrumentId(42ULL).seqNum(7ULL);

    MessageHeader hdr(buf, kSz);
    MdSubscriptionHeartbeat dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.timestampNs(), 88888ULL);
    EXPECT_EQ(dec.instrumentId(), 42ULL);
    EXPECT_EQ(dec.seqNum(), 7ULL);
}

// ── MdMarketData ─────────────────────────────────────────────────────────────

TEST(MdMarketDataTest, TemplateId) {
    EXPECT_EQ(MdMarketData::sbeTemplateId(), 7u);
}

TEST(MdMarketDataTest, EncodeDecodeAllFields) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + MdMarketData::sbeBlockLength();
    char buf[kSz]{};

    MdMarketData msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .timestampNs(123456789ULL)
        .instrumentId(55ULL)
        .bidPrice(29990.5)
        .bidQty(1.25)
        .askPrice(29991.0)
        .askQty(0.75)
        .seqNum(100ULL);

    MessageHeader hdr(buf, kSz);
    MdMarketData dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.timestampNs(), 123456789ULL);
    EXPECT_EQ(dec.instrumentId(), 55ULL);
    EXPECT_DOUBLE_EQ(dec.bidPrice(), 29990.5);
    EXPECT_DOUBLE_EQ(dec.bidQty(), 1.25);
    EXPECT_DOUBLE_EQ(dec.askPrice(), 29991.0);
    EXPECT_DOUBLE_EQ(dec.askQty(), 0.75);
    EXPECT_EQ(dec.seqNum(), 100ULL);
}

TEST(MdMarketDataTest, ZeroPrices) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + MdMarketData::sbeBlockLength();
    char buf[kSz]{};

    MdMarketData msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .timestampNs(0ULL)
        .instrumentId(0ULL)
        .bidPrice(0.0)
        .bidQty(0.0)
        .askPrice(0.0)
        .askQty(0.0)
        .seqNum(0ULL);

    MessageHeader hdr(buf, kSz);
    MdMarketData dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_DOUBLE_EQ(dec.bidPrice(), 0.0);
    EXPECT_DOUBLE_EQ(dec.askPrice(), 0.0);
}

// ── MdTrade ──────────────────────────────────────────────────────────────────

TEST(MdTradeTest, TemplateId) {
    EXPECT_EQ(MdTrade::sbeTemplateId(), 8u);
}

TEST(MdTradeTest, EncodeDecodeBuySide) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + MdTrade::sbeBlockLength();
    char buf[kSz]{};

    MdTrade msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .timestampNs(987654321ULL)
        .instrumentId(55ULL)
        .price(30000.0)
        .qty(0.5)
        .side(TradeSide::BUY)
        .seqNum(101ULL);

    MessageHeader hdr(buf, kSz);
    MdTrade dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.timestampNs(), 987654321ULL);
    EXPECT_EQ(dec.instrumentId(), 55ULL);
    EXPECT_DOUBLE_EQ(dec.price(), 30000.0);
    EXPECT_DOUBLE_EQ(dec.qty(), 0.5);
    EXPECT_EQ(dec.side(), TradeSide::BUY);
    EXPECT_EQ(dec.seqNum(), 101ULL);
}

TEST(MdTradeTest, EncodeDecodeSellSide) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + MdTrade::sbeBlockLength();
    char buf[kSz]{};

    MdTrade msg;
    msg.wrapAndApplyHeader(buf, 0, kSz)
        .timestampNs(1ULL)
        .instrumentId(1ULL)
        .price(100.0)
        .qty(2.0)
        .side(TradeSide::SELL)
        .seqNum(1ULL);

    MessageHeader hdr(buf, kSz);
    MdTrade dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.side(), TradeSide::SELL);
}

TEST(MdTradeTest, TradeSideValues) {
    EXPECT_EQ(static_cast<uint8_t>(TradeSide::BUY), 0u);
    EXPECT_EQ(static_cast<uint8_t>(TradeSide::SELL), 1u);
}

// ── MdServiceHeartbeat ───────────────────────────────────────────────────────

TEST(MdServiceHeartbeatTest, TemplateId) {
    EXPECT_EQ(MdServiceHeartbeat::sbeTemplateId(), 9u);
}

TEST(MdServiceHeartbeatTest, EncodeDecodeAllFields) {
    constexpr std::size_t kSz = MessageHeader::encodedLength() + MdServiceHeartbeat::sbeBlockLength();
    char buf[kSz]{};

    MdServiceHeartbeat msg;
    msg.wrapAndApplyHeader(buf, 0, kSz).timestampNs(555555ULL).seqNum(33ULL);

    MessageHeader hdr(buf, kSz);
    MdServiceHeartbeat dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kSz);

    EXPECT_EQ(dec.timestampNs(), 555555ULL);
    EXPECT_EQ(dec.seqNum(), 33ULL);
}

}  // namespace
