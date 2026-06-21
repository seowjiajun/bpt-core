#include <canon/canon_format.h>
#include <canon/canon_reader.h>
#include <canon/canon_sbe.h>
#include <canon/canon_writer.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

namespace bpt::canon::test {

namespace {

std::string make_tmp_path(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "bpt-canon-test";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

struct SyntheticEvent {
    uint64_t ts_ns;
    EventType type;
    std::vector<uint8_t> sbe;
};

std::vector<SyntheticEvent> make_events(std::size_t n) {
    std::mt19937 rng{42};
    std::uniform_int_distribution<int> blob_size{8, 256};
    std::uniform_int_distribution<int> byte_val{0, 255};

    std::vector<SyntheticEvent> out;
    out.reserve(n);
    uint64_t ts = 1'700'000'000'000'000'000ULL;
    const EventType types[] = {EventType::BBO, EventType::TRADE, EventType::BOOK, EventType::FUNDING};

    for (std::size_t i = 0; i < n; ++i) {
        SyntheticEvent ev;
        ev.ts_ns = ts;
        ev.type = types[i % 4];
        ev.sbe.resize(static_cast<std::size_t>(blob_size(rng)));
        for (auto& b : ev.sbe)
            b = static_cast<uint8_t>(byte_val(rng));
        out.push_back(std::move(ev));
        ts += 1'000;
    }
    return out;
}

}  // namespace

TEST(CanonRoundtrip, FileHeaderSizes) {
    EXPECT_EQ(sizeof(CanonFileHeader), kFileHeaderBytes);
    EXPECT_EQ(sizeof(CanonRecordHeader), 11U);
}

TEST(CanonRoundtrip, WriteAndReadBack) {
    const std::string path = make_tmp_path("roundtrip.canon");
    std::filesystem::remove(path);

    const auto events = make_events(1024);

    {
        CanonWriter::Config cfg;
        cfg.path = path;
        cfg.producer_kind = "unit-test";
        cfg.producer_sha = "0000000000000000000000000000000000000000";
        cfg.venue_id = 7;  // arbitrary
        cfg.date_utc = pack_date(2026, 5, 20);
        CanonWriter w(cfg);
        ASSERT_TRUE(w.open()) << "writer failed to open " << path;
        for (const auto& ev : events) {
            std::string_view blob(reinterpret_cast<const char*>(ev.sbe.data()), ev.sbe.size());
            ASSERT_TRUE(w.write_event(ev.ts_ns, ev.type, blob));
        }
        w.close();
        EXPECT_EQ(w.events_written(), events.size());
    }

    CanonReader r(path);
    ASSERT_TRUE(r.ok()) << "reader rejected file " << path;

    const auto& hdr = r.header();
    EXPECT_EQ(0, std::memcmp(hdr.magic, kMagic, sizeof(kMagic)));
    EXPECT_EQ(kSchemaVersion, hdr.schema_version);
    EXPECT_EQ(kSbeTemplateFamily, hdr.sbe_template_id);
    EXPECT_EQ(7U, hdr.venue_id);
    EXPECT_EQ(pack_date(2026, 5, 20), hdr.date_utc);
    EXPECT_EQ(0, std::memcmp(hdr.producer_kind, "unit-test", 9));

    for (const auto& expected : events) {
        auto got = r.next();
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(expected.ts_ns, got->ts_ns);
        EXPECT_EQ(expected.type, got->type);
        ASSERT_EQ(expected.sbe.size(), got->sbe.size());
        EXPECT_EQ(0, std::memcmp(expected.sbe.data(), got->sbe.data(), expected.sbe.size()));
    }
    EXPECT_FALSE(r.next().has_value()) << "extra records after expected EOF";
}

TEST(CanonRoundtrip, PathologicalLargeRecordTakesDirectPath) {
    // Force buffer-overflow path: 1 KiB buffer, single 50 KiB record. Stays
    // under UINT16_MAX so the sbe_len field accepts it, but exceeds the
    // userspace buffer so the direct-write branch in CanonWriter fires.
    const std::string path = make_tmp_path("big.canon");
    std::filesystem::remove(path);

    SyntheticEvent ev;
    ev.ts_ns = 1'234'567ULL;
    ev.type = EventType::BOOK;
    ev.sbe.assign(50 * 1024, 0xAB);

    {
        CanonWriter::Config cfg;
        cfg.path = path;
        cfg.producer_kind = "unit-test";
        cfg.venue_id = 1;
        cfg.date_utc = pack_date(2026, 1, 1);
        cfg.buffer_bytes = 1024;  // smaller than the record
        CanonWriter w(cfg);
        ASSERT_TRUE(w.open());
        std::string_view blob(reinterpret_cast<const char*>(ev.sbe.data()), ev.sbe.size());
        ASSERT_TRUE(w.write_event(ev.ts_ns, ev.type, blob));
    }

    CanonReader r(path);
    ASSERT_TRUE(r.ok());
    auto got = r.next();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(ev.ts_ns, got->ts_ns);
    EXPECT_EQ(ev.type, got->type);
    ASSERT_EQ(ev.sbe.size(), got->sbe.size());
    EXPECT_EQ(0, std::memcmp(ev.sbe.data(), got->sbe.data(), ev.sbe.size()));
    EXPECT_FALSE(r.next().has_value());
}

TEST(CanonRoundtrip, WriterRejectsOversizeRecord) {
    // sbe_len is uint16 on disk — records over 64 KiB must fail loudly at
    // write time. Real SBE templates (MdOrderBook with depth=20 etc.) are
    // well under this limit; if it ever fires, the schema has drifted.
    const std::string path = make_tmp_path("oversize.canon");
    std::filesystem::remove(path);

    CanonWriter::Config cfg;
    cfg.path = path;
    cfg.producer_kind = "unit-test";
    cfg.venue_id = 1;
    cfg.date_utc = pack_date(2026, 1, 1);
    CanonWriter w(cfg);
    ASSERT_TRUE(w.open());

    std::vector<uint8_t> huge(70 * 1024, 0xCD);
    std::string_view blob(reinterpret_cast<const char*>(huge.data()), huge.size());
    EXPECT_FALSE(w.write_event(1, EventType::BOOK, blob));
    EXPECT_EQ(w.events_written(), 0U);
}

TEST(CanonRoundtrip, ReaderRejectsBadMagic) {
    const std::string path = make_tmp_path("bad_magic.canon");
    std::filesystem::remove(path);

    // Hand-write a header with corrupt magic so we know the reader's
    // validation path actually fires (not just "file was empty").
    {
        std::FILE* fp = std::fopen(path.c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        CanonFileHeader hdr{};
        hdr.magic[0] = 'X';  // intentionally wrong
        hdr.schema_version = kSchemaVersion;
        hdr.sbe_template_id = kSbeTemplateFamily;
        std::fwrite(&hdr, sizeof(hdr), 1, fp);
        std::fclose(fp);
    }

    CanonReader r(path);
    EXPECT_FALSE(r.ok());
}

TEST(CanonRoundtrip, ReaderRejectsNewerSchema) {
    const std::string path = make_tmp_path("future_schema.canon");
    std::filesystem::remove(path);

    {
        std::FILE* fp = std::fopen(path.c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        CanonFileHeader hdr{};
        std::memcpy(hdr.magic, kMagic, sizeof(kMagic));
        hdr.schema_version = kSchemaVersion + 1;  // pretend a future writer wrote this
        hdr.sbe_template_id = kSbeTemplateFamily;
        std::fwrite(&hdr, sizeof(hdr), 1, fp);
        std::fclose(fp);
    }

    CanonReader r(path);
    EXPECT_FALSE(r.ok()) << "reader must refuse files newer than its schema version";
}

TEST(CanonSbeRoundtrip, BboEncodeDecode) {
    bpt::md_gateway::md::MdBbo in;
    in.timestamp_ns = 1'700'000'000'000'000'000ULL;
    in.instrument_id = 123;
    in.bid_price = 42.50;
    in.bid_qty = 100.0;
    in.ask_price = 42.75;
    in.ask_qty = 50.0;

    char buf[CanonScratch::kBboSize];
    const std::size_t n = encode_bbo(in, 7, buf, sizeof(buf));
    ASSERT_GT(n, 0U);

    bpt::md_gateway::md::MdBbo out{};
    ASSERT_TRUE(decode_bbo(buf, n, out));
    EXPECT_EQ(in.timestamp_ns, out.timestamp_ns);
    EXPECT_EQ(in.instrument_id, out.instrument_id);
    EXPECT_DOUBLE_EQ(in.bid_price, out.bid_price);
    EXPECT_DOUBLE_EQ(in.bid_qty, out.bid_qty);
    EXPECT_DOUBLE_EQ(in.ask_price, out.ask_price);
    EXPECT_DOUBLE_EQ(in.ask_qty, out.ask_qty);
}

TEST(CanonSbeRoundtrip, TradeEncodeDecode) {
    bpt::md_gateway::md::MdTrade in;
    in.timestamp_ns = 1'700'000'000'000'000'001ULL;
    in.instrument_id = 456;
    in.price = 1.234;
    in.qty = 7.0;
    in.side = bpt::messages::TradeSide::SELL;

    char buf[CanonScratch::kTradeSize];
    const std::size_t n = encode_trade(in, 9, buf, sizeof(buf));
    ASSERT_GT(n, 0U);

    bpt::md_gateway::md::MdTrade out{};
    ASSERT_TRUE(decode_trade(buf, n, out));
    EXPECT_EQ(in.timestamp_ns, out.timestamp_ns);
    EXPECT_EQ(in.instrument_id, out.instrument_id);
    EXPECT_DOUBLE_EQ(in.price, out.price);
    EXPECT_DOUBLE_EQ(in.qty, out.qty);
    EXPECT_EQ(in.side, out.side);
}

TEST(CanonSbeRoundtrip, BookEncodeDecode) {
    bpt::md_gateway::md::MdOrderBook in;
    in.timestamp_ns = 1'700'000'000'000'000'002ULL;
    in.instrument_id = 789;
    in.bids.emplace_back(100.0, 1.0);
    in.bids.emplace_back(99.5, 2.0);
    in.bids.emplace_back(99.0, 3.0);
    in.asks.emplace_back(100.5, 4.0);
    in.asks.emplace_back(101.0, 5.0);

    char buf[CanonScratch::kBookSize];
    const std::size_t n = encode_book(in, 11, buf, sizeof(buf));
    ASSERT_GT(n, 0U);

    bpt::md_gateway::md::MdOrderBook out{};
    ASSERT_TRUE(decode_book(buf, n, out));
    EXPECT_EQ(in.timestamp_ns, out.timestamp_ns);
    EXPECT_EQ(in.instrument_id, out.instrument_id);
    ASSERT_EQ(in.bids.size(), out.bids.size());
    for (std::size_t i = 0; i < in.bids.size(); ++i) {
        EXPECT_DOUBLE_EQ(in.bids[i].first, out.bids[i].first);
        EXPECT_DOUBLE_EQ(in.bids[i].second, out.bids[i].second);
    }
    ASSERT_EQ(in.asks.size(), out.asks.size());
    for (std::size_t i = 0; i < in.asks.size(); ++i) {
        EXPECT_DOUBLE_EQ(in.asks[i].first, out.asks[i].first);
        EXPECT_DOUBLE_EQ(in.asks[i].second, out.asks[i].second);
    }
}

}  // namespace bpt::canon::test
