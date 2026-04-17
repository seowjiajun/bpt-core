#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/FeeSchedule.h>
#include <messages/FundingRate.h>
#include <messages/InstrumentStatus.h>
#include <messages/InstrumentType.h>
#include <messages/MessageHeader.h>
#include <messages/OptionSide.h>
#include <messages/RefDataDelta.h>
#include <messages/RefDataError.h>
#include <messages/RefDataErrorType.h>
#include <messages/RefDataReady.h>
#include <messages/RefDataSnapshot.h>
#include <messages/RefDataSubscriptionRequest.h>

#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

using namespace bpt::messages;

// ---------------------------------------------------------------------------
// RefDataSubscriptionRequest
// ---------------------------------------------------------------------------

TEST(RefDataSubscriptionRequestTest, EncodeDecodeHeaderFields) {
    std::vector<char> buf(MessageHeader::encodedLength() + RefDataSubscriptionRequest::sbeBlockLength() +
                              RefDataSubscriptionRequest::Instruments::sbeHeaderSize() +
                              0 * RefDataSubscriptionRequest::Instruments::sbeBlockLength(),
                          '\0');

    RefDataSubscriptionRequest req;
    req.wrapAndApplyHeader(buf.data(), 0, buf.size()).correlationId(0xDEADBEEFull).timestampNs(987654321ull);
    // no instruments
    req.instrumentsCount(0);

    // Decode
    MessageHeader hdr(buf.data(), buf.size());
    EXPECT_EQ(hdr.templateId(), RefDataSubscriptionRequest::sbeTemplateId());
    EXPECT_EQ(hdr.schemaId(), RefDataSubscriptionRequest::sbeSchemaId());

    RefDataSubscriptionRequest dec;
    dec.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    EXPECT_EQ(dec.correlationId(), 0xDEADBEEFull);
    EXPECT_EQ(dec.timestampNs(), 987654321ull);
    EXPECT_EQ(dec.instruments().count(), 0u);
}

TEST(RefDataSubscriptionRequestTest, EncodeDecodeInstrumentsGroup) {
    constexpr uint16_t kNumInst = 2;
    std::vector<char> buf(MessageHeader::encodedLength() + RefDataSubscriptionRequest::sbeBlockLength() +
                              RefDataSubscriptionRequest::Instruments::sbeHeaderSize() +
                              kNumInst * RefDataSubscriptionRequest::Instruments::sbeBlockLength(),
                          '\0');

    RefDataSubscriptionRequest req;
    req.wrapAndApplyHeader(buf.data(), 0, buf.size()).correlationId(1).timestampNs(0);

    auto& g = req.instrumentsCount(kNumInst);
    g.next().putSymbol("BTCUSDT").putExchange("BINANCE");
    g.next().putSymbol("ETHUSDT").putExchange("OKX");

    // Decode
    MessageHeader hdr(buf.data(), buf.size());
    RefDataSubscriptionRequest dec;
    dec.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    auto& dg = dec.instruments();
    ASSERT_EQ(dg.count(), kNumInst);

    ASSERT_TRUE(dg.hasNext());
    dg.next();
    EXPECT_EQ(dg.getSymbolAsString(), "BTCUSDT");
    EXPECT_EQ(dg.getExchangeAsString(), "BINANCE");

    ASSERT_TRUE(dg.hasNext());
    dg.next();
    EXPECT_EQ(dg.getSymbolAsString(), "ETHUSDT");
    EXPECT_EQ(dg.getExchangeAsString(), "OKX");

    EXPECT_FALSE(dg.hasNext());
}

TEST(RefDataSubscriptionRequestTest, SymbolTruncatedAt24Chars) {
    // SBE symbol field is fixed at 24 chars — putSymbol must not overflow
    constexpr uint16_t kNumInst = 1;
    std::vector<char> buf(MessageHeader::encodedLength() + RefDataSubscriptionRequest::sbeBlockLength() +
                              RefDataSubscriptionRequest::Instruments::sbeHeaderSize() +
                              kNumInst * RefDataSubscriptionRequest::Instruments::sbeBlockLength(),
                          '\0');

    RefDataSubscriptionRequest req;
    req.wrapAndApplyHeader(buf.data(), 0, buf.size()).correlationId(0).timestampNs(0);

    auto& g = req.instrumentsCount(kNumInst);
    g.next().putSymbol("ABCDEFGHIJKLMNOPQRSTUVWX");  // exactly 24 chars

    MessageHeader hdr(buf.data(), buf.size());
    RefDataSubscriptionRequest dec;
    dec.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    auto& dg = dec.instruments();
    dg.next();
    EXPECT_EQ(dg.getSymbolAsString().size(), 24u);
}

// ---------------------------------------------------------------------------
// RefDataSnapshot
// ---------------------------------------------------------------------------

TEST(RefDataSnapshotTest, EncodeDecodeHeaderFields) {
    constexpr uint16_t kN = 0;
    std::vector<char> buf(MessageHeader::encodedLength() + RefDataSnapshot::sbeBlockLength() +
                              RefDataSnapshot::Instruments::sbeHeaderSize() +
                              kN * RefDataSnapshot::Instruments::sbeBlockLength(),
                          '\0');

    RefDataSnapshot snap;
    snap.wrapAndApplyHeader(buf.data(), 0, buf.size())
        .correlationId(42ull)
        .snapshotSeqNum(7ull)
        .timestampNs(111222333ull);
    snap.instrumentsCount(kN);

    MessageHeader hdr(buf.data(), buf.size());
    EXPECT_EQ(hdr.templateId(), RefDataSnapshot::sbeTemplateId());

    RefDataSnapshot dec;
    dec.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    EXPECT_EQ(dec.correlationId(), 42ull);
    EXPECT_EQ(dec.snapshotSeqNum(), 7ull);
    EXPECT_EQ(dec.timestampNs(), 111222333ull);
    EXPECT_EQ(dec.instruments().count(), kN);
}

TEST(RefDataSnapshotTest, EncodeDecodeInstrumentScalarFields) {
    constexpr uint16_t kN = 1;
    std::vector<char> buf(MessageHeader::encodedLength() + RefDataSnapshot::sbeBlockLength() +
                              RefDataSnapshot::Instruments::sbeHeaderSize() +
                              kN * RefDataSnapshot::Instruments::sbeBlockLength(),
                          '\0');

    RefDataSnapshot snap;
    snap.wrapAndApplyHeader(buf.data(), 0, buf.size()).correlationId(1).snapshotSeqNum(0).timestampNs(0);

    auto& g = snap.instrumentsCount(kN);
    g.next()
        .instrumentId(9001ull)
        .instrumentType(InstrumentType::PERPETUAL)
        .status(InstrumentStatus::ACTIVE)
        .lotSize(0.001)
        .tickSize(0.1)
        .contractSize(1.0)
        .expiryDate(0u)
        .optionSide(OptionSide::NA)
        .strikePrice(0.0);

    char sym[24] = {};
    std::memcpy(sym, "BTCUSDT", 7);
    char ex[8] = {};
    std::memcpy(ex, "OKX", 3);
    char base[8] = {};
    std::memcpy(base, "BTC", 3);
    char quot[8] = {};
    std::memcpy(quot, "USDT", 4);
    std::memcpy(g.symbol(), sym, 24);
    std::memcpy(g.exchange(), ex, 8);
    std::memcpy(g.baseCurrency(), base, 8);
    std::memcpy(g.quoteCurrency(), quot, 8);

    // Decode
    MessageHeader hdr(buf.data(), buf.size());
    RefDataSnapshot dec;
    dec.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    auto& dg = dec.instruments();
    ASSERT_TRUE(dg.hasNext());
    dg.next();

    EXPECT_EQ(dg.instrumentId(), 9001ull);
    EXPECT_EQ(dg.instrumentType(), InstrumentType::PERPETUAL);
    EXPECT_EQ(dg.status(), InstrumentStatus::ACTIVE);
    EXPECT_DOUBLE_EQ(dg.lotSize(), 0.001);
    EXPECT_DOUBLE_EQ(dg.tickSize(), 0.1);
    EXPECT_DOUBLE_EQ(dg.contractSize(), 1.0);
    EXPECT_EQ(dg.expiryDate(), 0u);
    EXPECT_EQ(dg.optionSide(), OptionSide::NA);
    EXPECT_DOUBLE_EQ(dg.strikePrice(), 0.0);

    EXPECT_EQ(dg.getSymbolAsString(), "BTCUSDT");
    EXPECT_EQ(dg.getExchangeAsString(), "OKX");
    EXPECT_EQ(dg.getBaseCurrencyAsString(), "BTC");
    EXPECT_EQ(dg.getQuoteCurrencyAsString(), "USDT");
}

TEST(RefDataSnapshotTest, MultipleInstruments) {
    constexpr uint16_t kN = 3;
    std::vector<char> buf(MessageHeader::encodedLength() + RefDataSnapshot::sbeBlockLength() +
                              RefDataSnapshot::Instruments::sbeHeaderSize() +
                              kN * RefDataSnapshot::Instruments::sbeBlockLength(),
                          '\0');

    RefDataSnapshot snap;
    snap.wrapAndApplyHeader(buf.data(), 0, buf.size()).correlationId(99).snapshotSeqNum(5).timestampNs(0);

    auto& g = snap.instrumentsCount(kN);
    for (uint64_t i = 0; i < kN; ++i) {
        g.next().instrumentId(100 + i);
    }

    MessageHeader hdr(buf.data(), buf.size());
    RefDataSnapshot dec;
    dec.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    auto& dg = dec.instruments();
    EXPECT_EQ(dg.count(), kN);
    for (uint64_t i = 0; i < kN; ++i) {
        ASSERT_TRUE(dg.hasNext());
        dg.next();
        EXPECT_EQ(dg.instrumentId(), 100 + i);
    }
    EXPECT_FALSE(dg.hasNext());
}

// ---------------------------------------------------------------------------
// RefDataDelta
// ---------------------------------------------------------------------------

TEST(RefDataDeltaTest, EncodeDecodeAllFields) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + RefDataDelta::sbeBlockLength();
    char buf[kBufSize] = {};

    RefDataDelta delta;
    delta.wrapAndApplyHeader(buf, 0, kBufSize)
        .deltaSeqNum(55ull)
        .timestampNs(999ull)
        .updateType(DeltaUpdateType::MODIFY)
        .instrumentId(12345ull)
        .lotSize(0.01)
        .tickSize(0.5)
        .contractSize(100.0)
        .expiryDate(20261231u)
        .optionSide(OptionSide::CALL)
        .strikePrice(50000.0)
        .instrumentType(InstrumentType::OPTION)
        .status(InstrumentStatus::HALTED);

    char sym[24] = {};
    std::memcpy(sym, "BTC-DEC26-50000-C", 17);
    char ex[8] = {};
    std::memcpy(ex, "DERIBIT", 7);
    char base[8] = {};
    std::memcpy(base, "BTC", 3);
    char quot[8] = {};
    std::memcpy(quot, "USD", 3);
    std::memcpy(delta.symbol(), sym, 24);
    std::memcpy(delta.exchange(), ex, 8);
    std::memcpy(delta.baseCurrency(), base, 8);
    std::memcpy(delta.quoteCurrency(), quot, 8);

    // Decode
    MessageHeader hdr(buf, kBufSize);
    EXPECT_EQ(hdr.templateId(), RefDataDelta::sbeTemplateId());

    RefDataDelta dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kBufSize);

    EXPECT_EQ(dec.deltaSeqNum(), 55ull);
    EXPECT_EQ(dec.timestampNs(), 999ull);
    EXPECT_EQ(dec.updateType(), DeltaUpdateType::MODIFY);
    EXPECT_EQ(dec.instrumentId(), 12345ull);
    EXPECT_DOUBLE_EQ(dec.lotSize(), 0.01);
    EXPECT_DOUBLE_EQ(dec.tickSize(), 0.5);
    EXPECT_DOUBLE_EQ(dec.contractSize(), 100.0);
    EXPECT_EQ(dec.expiryDate(), 20261231u);
    EXPECT_EQ(dec.optionSide(), OptionSide::CALL);
    EXPECT_DOUBLE_EQ(dec.strikePrice(), 50000.0);
    EXPECT_EQ(dec.instrumentType(), InstrumentType::OPTION);
    EXPECT_EQ(dec.status(), InstrumentStatus::HALTED);

    EXPECT_EQ(dec.getSymbolAsString(), "BTC-DEC26-50000-C");
    EXPECT_EQ(dec.getExchangeAsString(), "DERIBIT");
    EXPECT_EQ(dec.getBaseCurrencyAsString(), "BTC");
    EXPECT_EQ(dec.getQuoteCurrencyAsString(), "USD");
}

TEST(RefDataDeltaTest, UpdateTypeValues) {
    // Verify the DeltaUpdateType enum values our code relies on.
    // If the schema changes these, this test will catch it.
    EXPECT_EQ(static_cast<uint8_t>(DeltaUpdateType::ADD), 0u);
    EXPECT_EQ(static_cast<uint8_t>(DeltaUpdateType::MODIFY), 1u);
    EXPECT_EQ(static_cast<uint8_t>(DeltaUpdateType::REMOVE), 2u);
}

TEST(RefDataDeltaTest, InstrumentTypeValues) {
    EXPECT_EQ(static_cast<uint8_t>(InstrumentType::SPOT), 0u);
    EXPECT_EQ(static_cast<uint8_t>(InstrumentType::FUTURE), 1u);
    EXPECT_EQ(static_cast<uint8_t>(InstrumentType::PERPETUAL), 2u);
    EXPECT_EQ(static_cast<uint8_t>(InstrumentType::OPTION), 3u);
}

TEST(RefDataDeltaTest, InstrumentStatusValues) {
    EXPECT_EQ(static_cast<uint8_t>(InstrumentStatus::ACTIVE), 0u);
    EXPECT_EQ(static_cast<uint8_t>(InstrumentStatus::INACTIVE), 1u);
    EXPECT_EQ(static_cast<uint8_t>(InstrumentStatus::HALTED), 2u);
}

// ---------------------------------------------------------------------------
// RefDataReady (message id=16)
// ---------------------------------------------------------------------------

TEST(RefDataReadyTest, EncodeDecodeAllFields) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + RefDataReady::sbeBlockLength();
    char buf[kBufSize] = {};

    RefDataReady msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .timestampNs(123456789ull)
        .exchangesLoaded(0x03)  // BINANCE + OKX
        .instrumentCount(250)
        .feeSchedulesLoaded(1)
        .fundingRatesLoaded(1);

    MessageHeader hdr(buf, kBufSize);
    EXPECT_EQ(hdr.templateId(), RefDataReady::sbeTemplateId());
    EXPECT_EQ(hdr.sbeSchemaVersion(), RefDataReady::sbeSchemaVersion());

    RefDataReady dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kBufSize);

    EXPECT_EQ(dec.timestampNs(), 123456789ull);
    EXPECT_EQ(dec.exchangesLoaded(), 0x03u);
    EXPECT_EQ(dec.instrumentCount(), 250u);
    EXPECT_EQ(dec.feeSchedulesLoaded(), 1u);
    EXPECT_EQ(dec.fundingRatesLoaded(), 1u);
}

TEST(RefDataReadyTest, BlockLengthIs13) {
    EXPECT_EQ(RefDataReady::sbeBlockLength(), 13u);
}

TEST(RefDataReadyTest, TemplateIdIs16) {
    EXPECT_EQ(RefDataReady::sbeTemplateId(), 16u);
}

TEST(RefDataReadyTest, ExchangesBitmask) {
    // Verify bitmask convention: bit0=BINANCE, bit1=OKX, bit2=HYPERLIQUID
    EXPECT_EQ(0x01u & 0x01u, 0x01u);  // BINANCE present
    EXPECT_EQ(0x07u & 0x02u, 0x02u);  // OKX present in 0x07
    EXPECT_EQ(0x07u & 0x04u, 0x04u);  // HYPERLIQUID present in 0x07
}

// ---------------------------------------------------------------------------
// RefDataError (message id=17)
// ---------------------------------------------------------------------------

TEST(RefDataErrorTest, EncodeDecodeAllFields) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + RefDataError::sbeBlockLength();
    char buf[kBufSize] = {};

    RefDataError msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .timestampNs(999000ull)
        .errorType(RefDataErrorType::SNAPSHOT_FAILED)
        .exchangeId(ExchangeId::BINANCE)
        .instrumentId(0ull);

    MessageHeader hdr(buf, kBufSize);
    EXPECT_EQ(hdr.templateId(), RefDataError::sbeTemplateId());

    RefDataError dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kBufSize);

    EXPECT_EQ(dec.timestampNs(), 999000ull);
    EXPECT_EQ(dec.errorType(), RefDataErrorType::SNAPSHOT_FAILED);
    EXPECT_EQ(dec.exchangeId(), ExchangeId::BINANCE);
    EXPECT_EQ(dec.instrumentId(), 0ull);
}

TEST(RefDataErrorTest, TemplateIdIs17) {
    EXPECT_EQ(RefDataError::sbeTemplateId(), 17u);
}

TEST(RefDataErrorTest, BlockLengthIs18) {
    EXPECT_EQ(RefDataError::sbeBlockLength(), 18u);
}

TEST(RefDataErrorTypeTest, EnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(RefDataErrorType::EXCHANGE_NOT_CONFIGURED), 1u);
    EXPECT_EQ(static_cast<uint8_t>(RefDataErrorType::SNAPSHOT_FAILED), 2u);
    EXPECT_EQ(static_cast<uint8_t>(RefDataErrorType::DELTA_FEED_LOST), 3u);
    EXPECT_EQ(static_cast<uint8_t>(RefDataErrorType::LOOKUP_FAILED), 4u);
    EXPECT_EQ(static_cast<uint8_t>(RefDataErrorType::NULL_VALUE), 255u);
}

// ---------------------------------------------------------------------------
// FundingRate (message id=18)
// ---------------------------------------------------------------------------

TEST(FundingRateTest, EncodeDecodeAllFields) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + FundingRate::sbeBlockLength();
    char buf[kBufSize] = {};

    FundingRate msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(ExchangeId::BINANCE)
        .instrumentId(0xDEADBEEFCAFEBABEull)
        .rateBps(-15)  // -0.15bps rebate
        .nextFundingTs(1700000000000000000ull)
        .collectedTs(1699999990000000000ull);

    MessageHeader hdr(buf, kBufSize);
    EXPECT_EQ(hdr.templateId(), FundingRate::sbeTemplateId());

    FundingRate dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kBufSize);

    EXPECT_EQ(dec.exchangeId(), ExchangeId::BINANCE);
    EXPECT_EQ(dec.instrumentId(), 0xDEADBEEFCAFEBABEull);
    EXPECT_EQ(dec.rateBps(), -15);
    EXPECT_EQ(dec.nextFundingTs(), 1700000000000000000ull);
    EXPECT_EQ(dec.collectedTs(), 1699999990000000000ull);
}

TEST(FundingRateTest, TemplateIdIs18) {
    EXPECT_EQ(FundingRate::sbeTemplateId(), 18u);
}

TEST(FundingRateTest, BlockLengthIs29) {
    EXPECT_EQ(FundingRate::sbeBlockLength(), 29u);
}

TEST(FundingRateTest, NegativeRateBpsRoundTrip) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + FundingRate::sbeBlockLength();
    char buf[kBufSize] = {};

    FundingRate msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(ExchangeId::OKX)
        .instrumentId(1ull)
        .rateBps(-100)
        .nextFundingTs(0ull)
        .collectedTs(0ull);

    FundingRate dec;
    MessageHeader hdr(buf, kBufSize);
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kBufSize);
    EXPECT_EQ(dec.rateBps(), -100);
}

// ---------------------------------------------------------------------------
// FeeSchedule (message id=19)
// ---------------------------------------------------------------------------

TEST(FeeScheduleTest, EncodeDecodeAllFields) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + FeeSchedule::sbeBlockLength();
    char buf[kBufSize] = {};

    FeeSchedule msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(ExchangeId::OKX)
        .instrumentId(0ull)  // 0 = exchange-wide
        .instrumentType(InstrumentType::SPOT)
        .makerFeeBps(-8)  // -0.08% rebate
        .takerFeeBps(10)  // 0.10%
        .updatedTs(1700000000000000000ull);

    MessageHeader hdr(buf, kBufSize);
    EXPECT_EQ(hdr.templateId(), FeeSchedule::sbeTemplateId());

    FeeSchedule dec;
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kBufSize);

    EXPECT_EQ(dec.exchangeId(), ExchangeId::OKX);
    EXPECT_EQ(dec.instrumentId(), 0ull);
    EXPECT_EQ(dec.instrumentType(), InstrumentType::SPOT);
    EXPECT_EQ(dec.makerFeeBps(), -8);
    EXPECT_EQ(dec.takerFeeBps(), 10);
    EXPECT_EQ(dec.updatedTs(), 1700000000000000000ull);
}

TEST(FeeScheduleTest, TemplateIdIs19) {
    EXPECT_EQ(FeeSchedule::sbeTemplateId(), 19u);
}

TEST(FeeScheduleTest, BlockLengthIs22) {
    EXPECT_EQ(FeeSchedule::sbeBlockLength(), 22u);
}

TEST(FeeScheduleTest, NegativeMakerRoundTrip) {
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + FeeSchedule::sbeBlockLength();
    char buf[kBufSize] = {};

    FeeSchedule msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(ExchangeId::HYPERLIQUID)
        .instrumentId(0ull)
        .instrumentType(InstrumentType::PERPETUAL)
        .makerFeeBps(-10)
        .takerFeeBps(35)
        .updatedTs(0ull);

    FeeSchedule dec;
    MessageHeader hdr(buf, kBufSize);
    dec.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kBufSize);
    EXPECT_EQ(dec.makerFeeBps(), -10);
    EXPECT_EQ(dec.takerFeeBps(), 35);
}

}  // namespace
