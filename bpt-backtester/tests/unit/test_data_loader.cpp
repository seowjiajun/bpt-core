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
        cfg.start = start;
        cfg.end = end;
        cfg.allow_partial_data = partial;
        return cfg;
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(DataLoaderTest, ReturnsTradeEvents) {
    auto tp = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-01");
    auto op = make_dir("orderbook", "BINANCE", "BTCUSDT", "2026-01-01");

    write_trades_parquet(tp, {1000, 3000}, {100.0, 101.0}, {0.1, 0.2}, {0, 1});
    write_orderbook_parquet(op, {2000});

    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-01", "2026-01-01"), {inst});

    std::vector<MarketEvent> events;
    while (auto ev = loader.next())
        events.push_back(*ev);

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].timestamp_ns, 1000u);
    EXPECT_EQ(events[0].type, MarketEvent::Type::TRADE);
    EXPECT_EQ(events[1].timestamp_ns, 2000u);
    EXPECT_EQ(events[1].type, MarketEvent::Type::ORDER_BOOK);
    EXPECT_EQ(events[2].timestamp_ns, 3000u);
    EXPECT_EQ(events[2].type, MarketEvent::Type::TRADE);
}

TEST_F(DataLoaderTest, MergesMultipleInstrumentsInOrder) {
    auto tp1 = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-01");
    auto op1 = make_dir("orderbook", "BINANCE", "BTCUSDT", "2026-01-01");
    auto tp2 = make_dir("trades", "BINANCE", "ETHUSDT", "2026-01-01");
    auto op2 = make_dir("orderbook", "BINANCE", "ETHUSDT", "2026-01-01");

    write_trades_parquet(tp1, {100, 300}, {1.0, 1.0}, {0.1, 0.1}, {0, 0});
    write_orderbook_parquet(op1, {500});
    write_trades_parquet(tp2, {200, 400}, {2.0, 2.0}, {0.2, 0.2}, {1, 1});
    write_orderbook_parquet(op2, {600});

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
    for (const std::string& date : {"2026-01-01", "2026-01-02"}) {
        auto tp = make_dir("trades", "BINANCE", "BTCUSDT", date);
        auto op = make_dir("orderbook", "BINANCE", "BTCUSDT", date);
        write_trades_parquet(tp, {1000}, {100.0}, {1.0}, {0});
        write_orderbook_parquet(op, {2000});
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
    write_trades_parquet(tp, {500}, {99.0}, {1.0}, {0});
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
    write_trades_parquet(tp, {999}, {42000.5}, {0.05}, {1});
    write_orderbook_parquet(op, {});

    InstrumentConfig inst{"OKX", "BTC-USDT-SWAP"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-05", "2026-01-05"), {inst});

    auto ev = loader.next();
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type, MarketEvent::Type::TRADE);

    const auto& t = std::get<TradeRecord>(ev->payload);
    EXPECT_EQ(t.timestamp_ns, 999u);
    EXPECT_DOUBLE_EQ(t.price, 42000.5);
    EXPECT_DOUBLE_EQ(t.quantity, 0.05);
    EXPECT_EQ(t.side, TradeSide::SELL);
    EXPECT_EQ(t.exchange, "OKX");
    EXPECT_EQ(t.symbol, "BTC-USDT-SWAP");
}

TEST_F(DataLoaderTest, OrderBookFieldsAreCorrect) {
    auto tp = make_dir("trades", "BINANCE", "BTCUSDT", "2026-01-05");
    auto op = make_dir("orderbook", "BINANCE", "BTCUSDT", "2026-01-05");
    write_trades_parquet(tp, {}, {}, {}, {});
    write_orderbook_parquet(op, {777});

    InstrumentConfig inst{"BINANCE", "BTCUSDT"};
    DataLoader loader(data_cfg(), sim_cfg("2026-01-05", "2026-01-05"), {inst});

    auto ev = loader.next();
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type, MarketEvent::Type::ORDER_BOOK);

    const auto& ob = std::get<OrderBookRecord>(ev->payload);
    EXPECT_EQ(ob.timestamp_ns, 777u);
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
