#include "backtester/data/data_loader.h"

#include <algorithm>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <chrono>
#include <filesystem>
#include <format>
#include <parquet/arrow/reader.h>
#include <stdexcept>
#include <vector>

namespace bpt::backtester::data {

namespace fs = std::filesystem;
using namespace std::chrono;

// ── Date helpers ─────────────────────────────────────────────────────────────

static sys_days parse_date(const std::string& iso8601) {
    // Accepts "YYYY-MM-DDTHH:MM:SSZ" or "YYYY-MM-DD"
    int y = std::stoi(iso8601.substr(0, 4));
    int m = std::stoi(iso8601.substr(5, 2));
    int d = std::stoi(iso8601.substr(8, 2));
    return sys_days{year{y} / month{static_cast<unsigned>(m)} / day{static_cast<unsigned>(d)}};
}

static std::string format_date(sys_days d) {
    year_month_day ymd{d};
    return std::format("{:04d}-{:02d}-{:02d}",
                       static_cast<int>(ymd.year()),
                       static_cast<unsigned>(ymd.month()),
                       static_cast<unsigned>(ymd.day()));
}

// ── Arrow helpers ─────────────────────────────────────────────────────────────

static std::shared_ptr<arrow::Table> read_parquet_table(const std::string& path) {
    auto result = arrow::io::ReadableFile::Open(path);
    if (!result.ok())
        throw std::runtime_error("Failed to open Parquet file: " + path + " — " + result.status().ToString());

    parquet::arrow::FileReaderBuilder builder;
    auto open_status = builder.Open(*result);
    if (!open_status.ok())
        throw std::runtime_error("Failed to open Parquet reader: " + path + " — " + open_status.ToString());

    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto build_status = builder.Build(&reader);
    if (!build_status.ok())
        throw std::runtime_error("Failed to build Parquet reader: " + path + " — " + build_status.ToString());

    std::shared_ptr<arrow::Table> table;
    auto status = reader->ReadTable(&table);
    if (!status.ok())
        throw std::runtime_error("Failed to read Parquet table: " + path + " — " + status.ToString());

    return table;
}

static const arrow::Int64Array* get_int64_column(const arrow::Table& table, const std::string& name) {
    auto col = table.GetColumnByName(name);
    if (!col || col->num_chunks() == 0)
        throw std::runtime_error("Missing column '" + name + "' in Parquet file");
    return static_cast<const arrow::Int64Array*>(col->chunk(0).get());
}

static const arrow::DoubleArray* get_double_column(const arrow::Table& table, const std::string& name) {
    auto col = table.GetColumnByName(name);
    if (!col || col->num_chunks() == 0)
        throw std::runtime_error("Missing column '" + name + "' in Parquet file");
    return static_cast<const arrow::DoubleArray*>(col->chunk(0).get());
}

static const arrow::Int8Array* get_int8_column(const arrow::Table& table, const std::string& name) {
    auto col = table.GetColumnByName(name);
    if (!col || col->num_chunks() == 0)
        throw std::runtime_error("Missing column '" + name + "' in Parquet file");
    return static_cast<const arrow::Int8Array*>(col->chunk(0).get());
}

// ── DataLoader ────────────────────────────────────────────────────────────────

DataLoader::DataLoader(const config::DataConfig& data_cfg,
                       const config::SimulationConfig& sim_cfg,
                       const std::vector<config::InstrumentConfig>& instruments)
    : local_cache_(data_cfg.local_cache),
      allow_partial_data_(sim_cfg.allow_partial_data),
      start_day_(parse_date(sim_cfg.start)),
      end_day_(parse_date(sim_cfg.end)) {
    for (const auto& inst : instruments) {
        InstrumentReader r;
        r.instrument = inst;
        r.current_day = start_day_;
        r.end_day = end_day_;
        readers_.push_back(std::move(r));
    }
}

std::string DataLoader::trades_path(const std::string& exchange, const std::string& symbol, sys_days day) const {
    return local_cache_ + "/trades/" + exchange + "/" + symbol + "/" + format_date(day) + ".parquet";
}

std::string DataLoader::orderbook_path(const std::string& exchange, const std::string& symbol, sys_days day) const {
    return local_cache_ + "/orderbook/" + exchange + "/" + symbol + "/" + format_date(day) + ".parquet";
}

void DataLoader::validate() {
    std::vector<std::string> missing;

    for (const auto& inst : readers_) {
        for (sys_days d = start_day_; d <= end_day_; d += days{1}) {
            auto tp = trades_path(inst.instrument.exchange, inst.instrument.symbol, d);
            auto op = orderbook_path(inst.instrument.exchange, inst.instrument.symbol, d);

            if (!fs::exists(tp))
                missing.push_back(tp);
            if (!fs::exists(op))
                missing.push_back(op);
        }
    }

    if (missing.empty()) {
        ygg::log::info("[DataLoader] All data files present");
        return;
    }

    if (allow_partial_data_) {
        for (const auto& p : missing)
            ygg::log::warn("[DataLoader] Missing (will skip): {}", p);
        return;
    }

    std::string msg = "[DataLoader] Missing data files:\n";
    for (const auto& p : missing)
        msg += "  " + p + "\n";
    throw std::runtime_error(msg);
}

std::vector<MarketEvent> DataLoader::read_trades(const std::string& path,
                                                 const std::string& exchange,
                                                 const std::string& symbol) const {
    auto table = read_parquet_table(path);
    auto* ts = get_int64_column(*table, "timestamp_ns");
    auto* px = get_double_column(*table, "price");
    auto* qty = get_double_column(*table, "quantity");
    auto* sd = get_int8_column(*table, "side");

    std::vector<MarketEvent> events;
    events.reserve(static_cast<std::size_t>(table->num_rows()));

    for (int64_t i = 0; i < table->num_rows(); ++i) {
        TradeRecord t;
        t.timestamp_ns = static_cast<uint64_t>(ts->Value(i));
        t.price = px->Value(i);
        t.quantity = qty->Value(i);
        t.side = static_cast<TradeSide>(sd->Value(i));
        t.exchange = exchange;
        t.symbol = symbol;
        events.push_back(MarketEvent::from_trade(std::move(t)));
    }

    return events;
}

std::vector<MarketEvent> DataLoader::read_orderbook(const std::string& path,
                                                    const std::string& exchange,
                                                    const std::string& symbol) const {
    auto table = read_parquet_table(path);
    auto* ts = get_int64_column(*table, "timestamp_ns");

    // Collect level column pointers upfront.
    const arrow::DoubleArray* bid_px[kOrderBookDepth];
    const arrow::DoubleArray* bid_sz[kOrderBookDepth];
    const arrow::DoubleArray* ask_px[kOrderBookDepth];
    const arrow::DoubleArray* ask_sz[kOrderBookDepth];

    for (int lvl = 0; lvl < kOrderBookDepth; ++lvl) {
        bid_px[lvl] = get_double_column(*table, "bid_px_" + std::to_string(lvl + 1));
        bid_sz[lvl] = get_double_column(*table, "bid_sz_" + std::to_string(lvl + 1));
        ask_px[lvl] = get_double_column(*table, "ask_px_" + std::to_string(lvl + 1));
        ask_sz[lvl] = get_double_column(*table, "ask_sz_" + std::to_string(lvl + 1));
    }

    std::vector<MarketEvent> events;
    events.reserve(static_cast<std::size_t>(table->num_rows()));

    for (int64_t i = 0; i < table->num_rows(); ++i) {
        OrderBookRecord ob;
        ob.timestamp_ns = static_cast<uint64_t>(ts->Value(i));
        ob.exchange = exchange;
        ob.symbol = symbol;
        for (int lvl = 0; lvl < kOrderBookDepth; ++lvl) {
            ob.bid_px[lvl] = bid_px[lvl]->Value(i);
            ob.bid_sz[lvl] = bid_sz[lvl]->Value(i);
            ob.ask_px[lvl] = ask_px[lvl]->Value(i);
            ob.ask_sz[lvl] = ask_sz[lvl]->Value(i);
        }
        events.push_back(MarketEvent::from_orderbook(std::move(ob)));
    }

    return events;
}

void DataLoader::load_day(InstrumentReader& reader) {
    reader.day_events.clear();
    reader.pos = 0;

    const auto& ex = reader.instrument.exchange;
    const auto& sym = reader.instrument.symbol;
    sys_days d = reader.current_day;

    auto tp = trades_path(ex, sym, d);
    auto op = orderbook_path(ex, sym, d);

    // Trades
    bool has_real_trades = false;
    if (fs::exists(tp)) {
        try {
            auto evs = read_trades(tp, ex, sym);
            reader.day_events.insert(reader.day_events.end(),
                                     std::make_move_iterator(evs.begin()),
                                     std::make_move_iterator(evs.end()));
            has_real_trades = true;
        } catch (const std::exception& e) {
            ygg::log::error("[DataLoader] Failed to read {}: {}", tp, e.what());
        }
    } else if (!allow_partial_data_) {
        ygg::log::warn("[DataLoader] Trades file missing: {}", tp);
    }

    // Order book
    if (fs::exists(op)) {
        try {
            auto evs = read_orderbook(op, ex, sym);
            reader.day_events.insert(reader.day_events.end(),
                                     std::make_move_iterator(evs.begin()),
                                     std::make_move_iterator(evs.end()));
        } catch (const std::exception& e) {
            ygg::log::error("[DataLoader] Failed to read {}: {}", op, e.what());
        }
    } else if (!allow_partial_data_) {
        ygg::log::warn("[DataLoader] Orderbook file missing: {}", op);
    }

    // When no real trades exist, generate one synthetic trade per orderbook snapshot
    // at the mid-price with unit quantity.  This gives trade-dependent strategies
    // (e.g. VWAP) a price stream equivalent to a mid-price moving average.
    if (!has_real_trades && !reader.day_events.empty()) {
        ygg::log::debug("[DataLoader] No trades for {}/{} on {} — synthesising from orderbook mid",
                        ex,
                        sym,
                        format_date(d));
        std::vector<MarketEvent> synthetic;
        synthetic.reserve(reader.day_events.size());
        for (const auto& ev : reader.day_events) {
            if (ev.type != MarketEvent::Type::ORDER_BOOK)
                continue;
            const auto& ob = std::get<OrderBookRecord>(ev.payload);
            if (ob.bid_px[0] <= 0.0 || ob.ask_px[0] <= 0.0)
                continue;
            TradeRecord t;
            t.timestamp_ns = ob.timestamp_ns;
            t.price = (ob.bid_px[0] + ob.ask_px[0]) * 0.5;
            t.quantity = 0.01;
            t.side = TradeSide::BUY;
            t.exchange = ex;
            t.symbol = sym;
            synthetic.push_back(MarketEvent::from_trade(std::move(t)));
        }
        reader.day_events.insert(reader.day_events.end(),
                                 std::make_move_iterator(synthetic.begin()),
                                 std::make_move_iterator(synthetic.end()));
    }

    // Merge-sort trades + orderbook by timestamp.
    std::sort(reader.day_events.begin(), reader.day_events.end(), [](const MarketEvent& a, const MarketEvent& b) {
        return a.timestamp_ns < b.timestamp_ns;
    });

    ygg::log::debug("[DataLoader] Loaded {} events for {}/{} on {}", reader.day_events.size(), ex, sym, format_date(d));
}

bool DataLoader::InstrumentReader::advance() {
    current_day += days{1};
    return current_day <= end_day;
}

bool DataLoader::load_next_day() const {
    return false;  // checked externally via heap state
}

std::optional<MarketEvent> DataLoader::next() {
    // On first call, load the first day for each reader and seed the heap.
    if (heap_.empty()) {
        for (std::size_t i = 0; i < readers_.size(); ++i) {
            auto& r = readers_[i];
            if (r.day_events.empty() && r.current_day <= r.end_day) {
                load_day(r);
            }
            if (r.has_next()) {
                heap_.emplace(r.peek().timestamp_ns, i);
            }
        }
    }

    while (!heap_.empty()) {
        auto [ts, idx] = heap_.top();
        heap_.pop();

        auto& r = readers_[idx];

        // Stale entry — reader has already moved past this timestamp.
        if (!r.has_next() || r.peek().timestamp_ns != ts) {
            if (r.has_next())
                heap_.emplace(r.peek().timestamp_ns, idx);
            continue;
        }

        MarketEvent ev = r.consume();

        // If current day exhausted, load next day and re-seed heap.
        if (!r.has_next()) {
            if (r.advance()) {
                load_day(r);
                if (r.has_next())
                    heap_.emplace(r.peek().timestamp_ns, idx);
            }
        } else {
            heap_.emplace(r.peek().timestamp_ns, idx);
        }

        return ev;
    }

    return std::nullopt;
}

}  // namespace bpt::backtester::data
