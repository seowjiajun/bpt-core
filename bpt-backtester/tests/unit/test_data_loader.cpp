// Unit tests for bpt::backtester::data::DataLoader
#include "backtester/data/data_loader.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace bpt::backtester::data;
using namespace bpt::backtester::config;

// ── Parquet test helpers ──────────────────────────────────────────────────────

static void write_trades_parquet(const std::string& path,
                                 const std::vector<uint64_t>& ts,
                                 const std::vector<double>& px,
                                 const std::vector<double>& qty,
                                 const std::vector<int8_t>& side) {
    int64_t n = static_cast<int64_t>(ts.size());

    arrow::Int64Builder ts_b;
    arrow::DoubleBuilder px_b, qty_b;
    arrow::Int8Builder side_b;

    ASSERT_TRUE(ts_b.AppendValues(reinterpret_cast<const int64_t*>(ts.data()), n).ok());
    ASSERT_TRUE(px_b.AppendValues(px.data(), n).ok());
    ASSERT_TRUE(qty_b.AppendValues(qty.data(), n).ok());
    ASSERT_TRUE(side_b.AppendValues(side.data(), n).ok());

    std::shared_ptr<arrow::Array> ts_arr, px_arr, qty_arr, side_arr;
    ASSERT_TRUE(ts_b.Finish(&ts_arr).ok());
    ASSERT_TRUE(px_b.Finish(&px_arr).ok());
    ASSERT_TRUE(qty_b.Finish(&qty_arr).ok());
    ASSERT_TRUE(side_b.Finish(&side_arr).ok());

    auto schema = arrow::schema({arrow::field("timestamp_ns", arrow::int64()),
                                 arrow::field("price", arrow::float64()),
                                 arrow::field("quantity", arrow::float64()),
                                 arrow::field("side", arrow::int8())});

    auto table = arrow::Table::Make(schema, {ts_arr, px_arr, qty_arr, side_arr});

    auto sink = *arrow::io::FileOutputStream::Open(path);
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), sink, n).ok());
}

static void write_orderbook_parquet(const std::string& path, const std::vector<uint64_t>& ts) {
    int64_t n = static_cast<int64_t>(ts.size());

    arrow::Int64Builder ts_b;
    ASSERT_TRUE(ts_b.AppendValues(reinterpret_cast<const int64_t*>(ts.data()), n).ok());
    std::shared_ptr<arrow::Array> ts_arr;
    ASSERT_TRUE(ts_b.Finish(&ts_arr).ok());

    std::vector<std::shared_ptr<arrow::Field>> fields = {arrow::field("timestamp_ns", arrow::int64())};
    std::vector<std::shared_ptr<arrow::Array>> arrays = {ts_arr};

    for (int lvl = 1; lvl <= kOrderBookDepth; ++lvl) {
        std::string sfx = std::to_string(lvl);
        for (const std::string& col : {"bid_px_", "bid_sz_", "ask_px_", "ask_sz_"}) {
            arrow::DoubleBuilder b;
            std::vector<double> vals(n, static_cast<double>(lvl));
            ASSERT_TRUE(b.AppendValues(vals.data(), n).ok());
            std::shared_ptr<arrow::Array> arr;
            ASSERT_TRUE(b.Finish(&arr).ok());
            fields.push_back(arrow::field(col + sfx, arrow::float64()));
            arrays.push_back(arr);
        }
    }

    auto table = arrow::Table::Make(arrow::schema(fields), arrays);
    auto sink = *arrow::io::FileOutputStream::Open(path);
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), sink, n).ok());
}

// ── Fixture ───────────────────────────────────────────────────────────────────

class DataLoaderTest : public ::testing::Test {
protected:
    fs::path cache_dir_;

    void SetUp() override {
        // Include PID so concurrent gtest_discover_tests processes don't share
        // the same directory and race on SetUp/TearDown.
        cache_dir_ = fs::temp_directory_path() / ("backtester_dl_test_" + std::to_string(getpid()));
        fs::remove_all(cache_dir_);
        fs::create_directories(cache_dir_);
    }

    void TearDown() override { fs::remove_all(cache_dir_); }

    // Creates the directory tree and returns the full file path.
    std::string make_dir(const std::string& kind,
                         const std::string& exchange,
                         const std::string& symbol,
                         const std::string& date) {
        fs::path dir = cache_dir_ / kind / exchange / symbol;
        fs::create_directories(dir);
        return (dir / (date + ".parquet")).string();
    }

    DataConfig data_cfg() const {
        DataConfig cfg;
        cfg.local_cache = cache_dir_.string();
        return cfg;
    }

    SimulationConfig sim_cfg(const std::string& start, const std::string& end, bool partial = false) const {
        SimulationConfig cfg;
        cfg.windows.push_back(TimeWindow{start, end});
        cfg.start = start;  // back-compat scalars (loader sets these too)
        cfg.end = end;
        cfg.allow_partial_data = partial;
        return cfg;
    }

    SimulationConfig sim_cfg_windows(std::vector<TimeWindow> ws, bool partial = false) const {
        SimulationConfig cfg;
        cfg.windows = std::move(ws);
        std::sort(cfg.windows.begin(), cfg.windows.end(),
                  [](const TimeWindow& a, const TimeWindow& b) { return a.start < b.start; });
        cfg.start = cfg.windows.front().start;
        cfg.end   = cfg.windows.back().end;
        cfg.allow_partial_data = partial;
        return cfg;
    }
};

// ── Date helpers ──────────────────────────────────────────────────────────────
// Epoch-ns anchors. Real tape data carries epoch nanoseconds; the DataLoader
// filters every event against [start_ns, end_ns), so test fixtures must use
// timestamps that fall within their configured date.

static constexpr uint64_t kJan1_2026_NsUtc = 1767225600000000000ULL;          // 00:00:00Z
static constexpr uint64_t kDayNs           = 86'400ULL * 1'000'000'000ULL;
static constexpr uint64_t epoch_ns_for_jan_2026(int day_of_month) {
    return kJan1_2026_NsUtc + static_cast<uint64_t>(day_of_month - 1) * kDayNs;
}
static constexpr uint64_t kJan1_2026_13h_NsUtc = kJan1_2026_NsUtc + 13ULL * 3600 * 1'000'000'000ULL;
static constexpr uint64_t kJan1_2026_14h_NsUtc = kJan1_2026_NsUtc + 14ULL * 3600 * 1'000'000'000ULL;

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(DataLoaderTest, ReturnsTradeEvents) {
    auto tp = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-01");
    auto op = make_dir("orderbook", "BINANCE", "BTCUSDT", "2026-01-01");

    const uint64_t base = epoch_ns_for_jan_2026(1);
    write_trades_parquet(tp, {base + 1000, base + 3000}, {100.0, 101.0}, {0.1, 0.2}, {0, 1});
    write_orderbook_parquet(op, {base + 2000});

    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-01"), {inst});

    std::vector<MarketEvent> events;
    while (auto ev = loader.next())
        events.push_back(*ev);

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].timestamp_ns, base + 1000);
    EXPECT_EQ(events[0].type, MarketEvent::Type::TRADE);
    EXPECT_EQ(events[1].timestamp_ns, base + 2000);
    EXPECT_EQ(events[1].type, MarketEvent::Type::ORDER_BOOK);
    EXPECT_EQ(events[2].timestamp_ns, base + 3000);
    EXPECT_EQ(events[2].type, MarketEvent::Type::TRADE);
}

TEST_F(DataLoaderTest, MergesMultipleInstrumentsInOrder) {
    auto tp1 = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-01");
    auto op1 = make_dir("orderbook", "BINANCE", "BTCUSDT", "2026-01-01");
    auto tp2 = make_dir("trades", "BINANCE", "ETHUSDT", "2026-01-01");
    auto op2 = make_dir("orderbook", "BINANCE", "ETHUSDT", "2026-01-01");

    const uint64_t base = epoch_ns_for_jan_2026(1);
    write_trades_parquet(tp1, {base + 100, base + 300}, {1.0, 1.0}, {0.1, 0.1}, {0, 0});
    write_orderbook_parquet(op1, {base + 500});
    write_trades_parquet(tp2, {base + 200, base + 400}, {2.0, 2.0}, {0.2, 0.2}, {1, 1});
    write_orderbook_parquet(op2, {base + 600});

    InstrumentConfig btc{"BINANCE", "BTCUSDT"};
    InstrumentConfig eth{"BINANCE", "ETHUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-01"), {btc, eth});

    std::vector<uint64_t> tss;
    while (auto ev = loader.next())
        tss.push_back(ev->timestamp_ns);

    ASSERT_EQ(tss.size(), 6u);
    EXPECT_TRUE(std::is_sorted(tss.begin(), tss.end()));
}

TEST_F(DataLoaderTest, SpansMultipleDays) {
    int day_of_month = 1;
    for (const std::string& date : {"2026-01-01", "2026-01-02"}) {
        auto tp = make_dir("trades", "BINANCE", "BTCUSDT", date);
        auto op = make_dir("orderbook", "BINANCE", "BTCUSDT", date);
        const uint64_t base = epoch_ns_for_jan_2026(day_of_month);
        write_trades_parquet(tp, {base + 1000}, {100.0}, {1.0}, {0});
        write_orderbook_parquet(op, {base + 2000});
        ++day_of_month;
    }

    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-02"), {inst});

    int count = 0;
    while (loader.next())
        ++count;
    EXPECT_EQ(count, 4);  // 1 trade + 1 ob per day × 2 days
}

TEST_F(DataLoaderTest, ValidateThrowsOnMissingFiles) {
    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-01"), {inst});
    EXPECT_THROW(loader.validate(), std::runtime_error);
}

TEST_F(DataLoaderTest, ValidatePassesWhenAllFilesPresent) {
    auto tp = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-01");
    auto op = make_dir("orderbook", "BINANCE", "BTCUSDT", "2026-01-01");
    write_trades_parquet(tp, {}, {}, {}, {});
    write_orderbook_parquet(op, {});

    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-01"), {inst});
    EXPECT_NO_THROW(loader.validate());
}

TEST_F(DataLoaderTest, AllowPartialDataSkipsMissingFiles) {
    // Only trades file present, no orderbook.
    auto tp = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-01");
    const uint64_t ts = epoch_ns_for_jan_2026(1) + 500;
    write_trades_parquet(tp, {ts}, {99.0}, {1.0}, {0});
    // orderbook dir not created on purpose

    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-01", true), {inst});
    EXPECT_NO_THROW(loader.validate());

    int count = 0;
    while (loader.next())
        ++count;
    EXPECT_EQ(count, 1);  // only the trade event
}

TEST_F(DataLoaderTest, TradeFieldsAreCorrect) {
    auto tp = make_dir("trades", "OKX", "BTC-USDT-SWAP", "2026-01-05");
    auto op = make_dir("orderbook", "OKX", "BTC-USDT-SWAP", "2026-01-05");
    const uint64_t ts = epoch_ns_for_jan_2026(5) + 999;
    write_trades_parquet(tp, {ts}, {42000.5}, {0.05}, {1});
    write_orderbook_parquet(op, {});

    InstrumentConfig inst{"OKX", "BTC-USDT-SWAP"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-05", "2026-01-05"), {inst});

    auto ev = loader.next();
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type, MarketEvent::Type::TRADE);

    const auto& t = std::get<TradeRecord>(ev->payload);
    EXPECT_EQ(t.timestamp_ns, ts);
    EXPECT_DOUBLE_EQ(t.price, 42000.5);
    EXPECT_DOUBLE_EQ(t.quantity, 0.05);
    EXPECT_EQ(t.side, TradeSide::SELL);
    EXPECT_EQ(t.exchange, "OKX");
    EXPECT_EQ(t.symbol, "BTC-USDT-SWAP");
}

TEST_F(DataLoaderTest, OrderBookFieldsAreCorrect) {
    auto tp = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-05");
    auto op = make_dir("orderbook", "BINANCE", "BTCUSDT", "2026-01-05");
    const uint64_t ts = epoch_ns_for_jan_2026(5) + 777;
    write_trades_parquet(tp, {}, {}, {}, {});
    write_orderbook_parquet(op, {ts});

    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-05", "2026-01-05"), {inst});

    auto ev = loader.next();
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type, MarketEvent::Type::ORDER_BOOK);

    const auto& ob = std::get<OrderBookRecord>(ev->payload);
    EXPECT_EQ(ob.timestamp_ns, ts);
    EXPECT_EQ(ob.exchange, "BINANCE");
    EXPECT_EQ(ob.symbol, "BTCUSDT");
    // write_orderbook_parquet fills each level with its level number
    for (int lvl = 0; lvl < kOrderBookDepth; ++lvl) {
        EXPECT_DOUBLE_EQ(ob.bid_px[lvl], static_cast<double>(lvl + 1));
        EXPECT_DOUBLE_EQ(ob.ask_px[lvl], static_cast<double>(lvl + 1));
    }
}

TEST_F(DataLoaderTest, ReturnsNulloptWhenExhausted) {
    auto tp = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-01");
    auto op = make_dir("orderbook", "BINANCE", "BTCUSDT", "2026-01-01");
    write_trades_parquet(tp, {100}, {1.0}, {1.0}, {0});
    write_orderbook_parquet(op, {});

    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-01"), {inst});

    loader.next();  // consume the one event
    EXPECT_FALSE(loader.next().has_value());
    EXPECT_FALSE(loader.next().has_value());  // idempotent
}

// ── Intra-day windowing (Phase 1) ─────────────────────────────────────────────

TEST_F(DataLoaderTest, FullDayWindowEmitsAllEvents) {
    // Sanity: a date-only window (existing API) still emits every event of the day.
    auto tp = make_dir("trades", "HYPERLIQUID", "APE", "2026-01-01");
    auto op = make_dir("orderbook", "HYPERLIQUID", "APE", "2026-01-01");
    write_trades_parquet(tp,
        {kJan1_2026_NsUtc + 1, kJan1_2026_13h_NsUtc + 5, kJan1_2026_14h_NsUtc + 10},
        {1.0, 1.0, 1.0}, {0.1, 0.1, 0.1}, {0, 0, 0});
    write_orderbook_parquet(op, {});

    InstrumentConfig inst{"HYPERLIQUID", "APE"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-01"), {inst});
    int count = 0;
    while (loader.next()) ++count;
    EXPECT_EQ(count, 3);
}

TEST_F(DataLoaderTest, IntraDayWindowFiltersOutsideEvents) {
    // 1pm-2pm window: only events with ts ∈ [13:00, 14:00) survive.
    auto tp = make_dir("trades", "HYPERLIQUID", "APE", "2026-01-01");
    auto op = make_dir("orderbook", "HYPERLIQUID", "APE", "2026-01-01");
    write_trades_parquet(tp,
        {
            kJan1_2026_NsUtc + 1,                     // 00:00 — out (before)
            kJan1_2026_13h_NsUtc - 1,                 // 12:59:59.999... — out
            kJan1_2026_13h_NsUtc,                     // 13:00:00 — in (inclusive)
            kJan1_2026_13h_NsUtc + 30LL * 60 * 1'000'000'000LL,  // 13:30 — in
            kJan1_2026_14h_NsUtc - 1,                 // 13:59:59.999... — in
            kJan1_2026_14h_NsUtc,                     // 14:00:00 — out (exclusive)
            kJan1_2026_14h_NsUtc + 1,                 // 14:00:00.000... — out
        },
        {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0},
        {0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1},
        {0, 0, 0, 0, 0, 0, 0});
    write_orderbook_parquet(op, {});

    InstrumentConfig inst{"HYPERLIQUID", "APE"};
    DataLoader loader(data_cfg(),
                      sim_cfg("2026-01-01T13:00:00Z", "2026-01-01T14:00:00Z"),
                      {inst});

    std::vector<uint64_t> tss;
    while (auto ev = loader.next()) tss.push_back(ev->timestamp_ns);
    ASSERT_EQ(tss.size(), 3u);
    EXPECT_EQ(tss[0], kJan1_2026_13h_NsUtc);
    EXPECT_EQ(tss[1], kJan1_2026_13h_NsUtc + 30LL * 60 * 1'000'000'000LL);
    EXPECT_EQ(tss[2], kJan1_2026_14h_NsUtc - 1);
}

TEST_F(DataLoaderTest, SubSecondWindowIsRespected) {
    // 100 µs window starting at 13:00:00.000 100µs.
    const uint64_t window_start = kJan1_2026_13h_NsUtc + 100'000ULL;       // +100 µs
    const uint64_t window_end   = kJan1_2026_13h_NsUtc + 200'000ULL;       // +200 µs

    auto tp = make_dir("trades", "HYPERLIQUID", "APE", "2026-01-01");
    auto op = make_dir("orderbook", "HYPERLIQUID", "APE", "2026-01-01");
    write_trades_parquet(tp,
        {window_start - 1, window_start, window_start + 50'000ULL, window_end - 1, window_end},
        {1.0, 1.0, 1.0, 1.0, 1.0},
        {0.1, 0.1, 0.1, 0.1, 0.1},
        {0, 0, 0, 0, 0});
    write_orderbook_parquet(op, {});

    InstrumentConfig inst{"HYPERLIQUID", "APE"};
    DataLoader loader(data_cfg(),
                      sim_cfg("2026-01-01T13:00:00.000100000Z",
                              "2026-01-01T13:00:00.000200000Z"),
                      {inst});

    int count = 0;
    while (loader.next()) ++count;
    EXPECT_EQ(count, 3);  // window_start, +50µs, window_end - 1
}

TEST_F(DataLoaderTest, MultiDayWindowSkipsEmptyFirstDay) {
    // Window covers an exact slice on day 2; day 1 is in the date range but
    // the window doesn't intersect it. The reader must advance past day 1
    // even though day 1 yields zero events after filtering.
    for (const std::string& date : {"2026-01-01", "2026-01-02"}) {
        auto tp = make_dir("trades", "HYPERLIQUID", "APE", date);
        auto op = make_dir("orderbook", "HYPERLIQUID", "APE", date);
        write_trades_parquet(tp, {}, {}, {}, {});
        write_orderbook_parquet(op, {});
    }
    // Add events only on day 2.
    {
        const uint64_t kJan2_2026_NsUtc = kJan1_2026_NsUtc + 86'400ULL * 1'000'000'000ULL;
        auto tp = make_dir("trades", "HYPERLIQUID", "APE", "2026-01-02");
        write_trades_parquet(tp,
            {kJan2_2026_NsUtc + 5LL * 3600 * 1'000'000'000LL,
             kJan2_2026_NsUtc + 6LL * 3600 * 1'000'000'000LL},
            {1.0, 1.0}, {0.1, 0.1}, {0, 0});
    }

    InstrumentConfig inst{"HYPERLIQUID", "APE"};
    DataLoader loader(data_cfg(),
                      sim_cfg("2026-01-02T05:00:00Z", "2026-01-02T07:00:00Z"),
                      {inst});

    int count = 0;
    while (loader.next()) ++count;
    EXPECT_EQ(count, 2);
}

TEST_F(DataLoaderTest, RejectsInvertedWindow) {
    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    EXPECT_THROW(DataLoader(data_cfg(),
                            sim_cfg("2026-01-02T00:00:00Z", "2026-01-01T00:00:00Z"),
                            {inst}),
                 std::runtime_error);
}

// ── Multi-window unions (Phase 2) ────────────────────────────────────────────

TEST_F(DataLoaderTest, TwoDisjointWindowsSameDayDropsBetween) {
    auto tp = make_dir("trades", "HYPERLIQUID", "APE", "2026-01-01");
    auto op = make_dir("orderbook", "HYPERLIQUID", "APE", "2026-01-01");
    write_orderbook_parquet(op, {});

    const uint64_t base = epoch_ns_for_jan_2026(1);
    const auto at = [&](int hh, int mm = 0) -> uint64_t {
        return base + (uint64_t(hh) * 3600 + uint64_t(mm) * 60) * 1'000'000'000ULL;
    };
    write_trades_parquet(tp,
        {at(8), at(9), at(10), at(11), at(12), at(13), at(14)},
        {1, 1, 1, 1, 1, 1, 1},
        {0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1},
        {0, 0, 0, 0, 0, 0, 0});

    InstrumentConfig inst{"HYPERLIQUID", "APE"};
    DataLoader loader(data_cfg(),
                      sim_cfg_windows({
                          {"2026-01-01T09:00:00Z", "2026-01-01T10:00:00Z"},
                          {"2026-01-01T13:00:00Z", "2026-01-01T14:00:00Z"},
                      }),
                      {inst});

    std::vector<uint64_t> tss;
    while (auto ev = loader.next()) tss.push_back(ev->timestamp_ns);
    // 09:00 in window 1; 10:00 excluded (window 1 is half-open); 11/12 dropped;
    // 13:00 in window 2; 14:00 excluded.
    ASSERT_EQ(tss.size(), 2u);
    EXPECT_EQ(tss[0], at(9));
    EXPECT_EQ(tss[1], at(13));
}

TEST_F(DataLoaderTest, TwoWindowsAcrossDifferentDays) {
    for (const std::string& date : {"2026-01-01", "2026-01-02", "2026-01-03"}) {
        auto tp = make_dir("trades", "HYPERLIQUID", "APE", date);
        auto op = make_dir("orderbook", "HYPERLIQUID", "APE", date);
        write_orderbook_parquet(op, {});
        write_trades_parquet(tp, {}, {}, {}, {});
    }
    // One event 13:30 on each of 01, 02, 03.
    for (int dom = 1; dom <= 3; ++dom) {
        auto tp = make_dir("trades", "HYPERLIQUID", "APE",
                           "2026-01-0" + std::to_string(dom));
        const uint64_t ts = epoch_ns_for_jan_2026(dom)
            + 13ULL * 3600 * 1'000'000'000ULL
            + 30ULL * 60 * 1'000'000'000ULL;
        write_trades_parquet(tp, {ts}, {1.0}, {0.1}, {0});
    }

    InstrumentConfig inst{"HYPERLIQUID", "APE"};
    // Window day 1 + window day 3, skip day 2 entirely.
    DataLoader loader(data_cfg(),
                      sim_cfg_windows({
                          {"2026-01-01T13:00:00Z", "2026-01-01T14:00:00Z"},
                          {"2026-01-03T13:00:00Z", "2026-01-03T14:00:00Z"},
                      }),
                      {inst});

    int count = 0;
    while (loader.next()) ++count;
    EXPECT_EQ(count, 2);  // day 2 event filtered out even though file is loaded
}

TEST_F(DataLoaderTest, OverlappingWindowsDoNotDoubleEmit) {
    auto tp = make_dir("trades", "HYPERLIQUID", "APE", "2026-01-01");
    auto op = make_dir("orderbook", "HYPERLIQUID", "APE", "2026-01-01");
    write_orderbook_parquet(op, {});
    const uint64_t base = epoch_ns_for_jan_2026(1);
    const uint64_t ts = base + 13ULL * 3600 * 1'000'000'000ULL + 30ULL * 60 * 1'000'000'000ULL;
    write_trades_parquet(tp, {ts}, {1.0}, {0.1}, {0});

    InstrumentConfig inst{"HYPERLIQUID", "APE"};
    DataLoader loader(data_cfg(),
                      sim_cfg_windows({
                          {"2026-01-01T13:00:00Z", "2026-01-01T14:00:00Z"},
                          {"2026-01-01T13:15:00Z", "2026-01-01T13:45:00Z"},  // strict subset
                      }),
                      {inst});
    int count = 0;
    while (loader.next()) ++count;
    EXPECT_EQ(count, 1);  // single event emitted once even though both windows match
}

TEST_F(DataLoaderTest, RejectsEmptyWindowsList) {
    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    SimulationConfig empty;  // no windows populated
    EXPECT_THROW(DataLoader(data_cfg(), empty, {inst}), std::runtime_error);
}
