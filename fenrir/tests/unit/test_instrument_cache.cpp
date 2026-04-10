#include "fenrir/refdata/instrument_cache.h"

#include <bifrost_protocol/DeltaUpdateType.h>
#include <bifrost_protocol/InstrumentStatus.h>
#include <bifrost_protocol/InstrumentType.h>
#include <bifrost_protocol/MessageHeader.h>
#include <bifrost_protocol/OptionSide.h>
#include <bifrost_protocol/RefDataDelta.h>
#include <bifrost_protocol/RefDataSnapshot.h>

#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

using namespace bifrost::protocol;
using fenrir::refdata::InstrumentCache;

// ---------------------------------------------------------------------------
// Helpers to encode snapshot / delta buffers
// ---------------------------------------------------------------------------

std::vector<char> make_snapshot(uint64_t correlation_id,
                                std::initializer_list<std::pair<uint64_t, const char*>> instruments) {
    const uint16_t n = static_cast<uint16_t>(instruments.size());
    std::size_t buf_size = MessageHeader::encodedLength() + RefDataSnapshot::sbeBlockLength() +
                           RefDataSnapshot::Instruments::sbeHeaderSize() +
                           n * RefDataSnapshot::Instruments::sbeBlockLength();

    std::vector<char> buf(buf_size, '\0');
    RefDataSnapshot msg;
    msg.wrapAndApplyHeader(buf.data(), 0, buf_size).correlationId(correlation_id).snapshotSeqNum(1).timestampNs(0);

    auto& g = msg.instrumentsCount(n);
    for (const auto& [id, sym] : instruments) {
        g.next()
            .instrumentId(id)
            .instrumentType(InstrumentType::SPOT)
            .status(InstrumentStatus::ACTIVE)
            .lotSize(0.001)
            .tickSize(0.01)
            .contractSize(1.0)
            .expiryDate(0)
            .optionSide(OptionSide::NA)
            .strikePrice(0.0);
        char sbuf[24] = {};
        std::memcpy(sbuf, sym, std::min(std::strlen(sym), std::size_t{24}));
        std::memcpy(g.symbol(), sbuf, 24);
        char ex[8] = {};
        std::memcpy(ex, "BINANCE", 7);
        std::memcpy(g.exchange(), ex, 8);
    }
    return buf;
}

std::vector<char> make_delta(uint64_t instrument_id, const char* symbol, DeltaUpdateType::Value update_type,
                            uint64_t seq = 2) {
    constexpr std::size_t kSize = MessageHeader::encodedLength() + RefDataDelta::sbeBlockLength();
    std::vector<char> buf(kSize, '\0');
    RefDataDelta msg;
    msg.wrapAndApplyHeader(buf.data(), 0, kSize)
        .deltaSeqNum(seq)
        .timestampNs(0)
        .updateType(update_type)
        .instrumentId(instrument_id)
        .instrumentType(InstrumentType::SPOT)
        .status(InstrumentStatus::ACTIVE)
        .lotSize(0.001)
        .tickSize(0.01)
        .contractSize(1.0)
        .expiryDate(0)
        .optionSide(OptionSide::NA)
        .strikePrice(0.0);
    char sbuf[24] = {};
    std::memcpy(sbuf, symbol, std::min(std::strlen(symbol), std::size_t{24}));
    std::memcpy(msg.symbol(), sbuf, 24);
    char ex[8] = {};
    std::memcpy(ex, "BINANCE", 7);
    std::memcpy(msg.exchange(), ex, 8);
    return buf;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(InstrumentCacheTest, EmptyBeforeSnapshot) {
    InstrumentCache cache;
    EXPECT_FALSE(cache.snapshot_received());
    EXPECT_EQ(cache.size(), 0u);
}

TEST(InstrumentCacheTest, ApplySnapshot) {
    auto buf = make_snapshot(1, {{100, "BTCUSDT"}, {101, "ETHUSDT"}});
    MessageHeader hdr(buf.data(), buf.size());
    RefDataSnapshot msg;
    msg.wrapForDecode(buf.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf.size());

    InstrumentCache cache;
    cache.apply_snapshot(msg);

    EXPECT_TRUE(cache.snapshot_received());
    EXPECT_EQ(cache.size(), 2u);

    auto inst = cache.get(100);
    ASSERT_TRUE(inst.has_value());
    EXPECT_EQ(inst->symbol, "BTCUSDT");
    EXPECT_EQ(inst->exchange, "BINANCE");
}

TEST(InstrumentCacheTest, SnapshotClearsOldEntries) {
    InstrumentCache cache;

    auto buf1 = make_snapshot(1, {{100, "BTCUSDT"}, {101, "ETHUSDT"}});
    {
        MessageHeader hdr(buf1.data(), buf1.size());
        RefDataSnapshot msg;
        msg.wrapForDecode(buf1.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf1.size());
        cache.apply_snapshot(msg);
    }
    EXPECT_EQ(cache.size(), 2u);

    auto buf2 = make_snapshot(1, {{200, "SOLUSDT"}});
    {
        MessageHeader hdr(buf2.data(), buf2.size());
        RefDataSnapshot msg;
        msg.wrapForDecode(buf2.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf2.size());
        cache.apply_snapshot(msg);
    }
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.get(100).has_value());
    EXPECT_TRUE(cache.get(200).has_value());
}

TEST(InstrumentCacheTest, DeltaAdd) {
    auto snap = make_snapshot(1, {{100, "BTCUSDT"}});
    MessageHeader hdr(snap.data(), snap.size());
    RefDataSnapshot smsg;
    smsg.wrapForDecode(snap.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), snap.size());
    InstrumentCache cache;
    cache.apply_snapshot(smsg);

    auto dbuf = make_delta(101, "ETHUSDT", DeltaUpdateType::ADD);
    MessageHeader dhdr(dbuf.data(), dbuf.size());
    RefDataDelta dmsg;
    dmsg.wrapForDecode(dbuf.data(), MessageHeader::encodedLength(), dhdr.blockLength(), dhdr.version(), dbuf.size());
    cache.apply_delta(dmsg);

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_TRUE(cache.get(101).has_value());
    EXPECT_EQ(cache.get(101)->symbol, "ETHUSDT");
}

TEST(InstrumentCacheTest, DeltaRemove) {
    auto snap = make_snapshot(1, {{100, "BTCUSDT"}, {101, "ETHUSDT"}});
    MessageHeader hdr(snap.data(), snap.size());
    RefDataSnapshot smsg;
    smsg.wrapForDecode(snap.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), snap.size());
    InstrumentCache cache;
    cache.apply_snapshot(smsg);

    auto dbuf = make_delta(100, "BTCUSDT", DeltaUpdateType::REMOVE);
    MessageHeader dhdr(dbuf.data(), dbuf.size());
    RefDataDelta dmsg;
    dmsg.wrapForDecode(dbuf.data(), MessageHeader::encodedLength(), dhdr.blockLength(), dhdr.version(), dbuf.size());
    cache.apply_delta(dmsg);

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.get(100).has_value());
    EXPECT_TRUE(cache.get(101).has_value());
}

TEST(InstrumentCacheTest, GetAllReturnsAllEntries) {
    auto snap = make_snapshot(1, {{1, "A"}, {2, "B"}, {3, "C"}});
    MessageHeader hdr(snap.data(), snap.size());
    RefDataSnapshot msg;
    msg.wrapForDecode(snap.data(), MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), snap.size());
    InstrumentCache cache;
    cache.apply_snapshot(msg);

    EXPECT_EQ(cache.get_all().size(), 3u);
}

}  // namespace
