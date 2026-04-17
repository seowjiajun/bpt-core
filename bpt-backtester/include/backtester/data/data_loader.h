#pragma once

#include "backtester/config/settings.h"
#include "backtester/data/market_event.h"

#include <chrono>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace bpt::backtester::data {

// DataLoader merges trade and order book events from Parquet files across all
// configured instruments, returning them in strict timestamp order.
//
// Usage:
//   DataLoader loader(data_cfg, sim_cfg, instruments);
//   loader.validate();           // throws on missing files
//   while (auto ev = loader.next()) { ... }
class DataLoader {
public:
    DataLoader(const config::DataConfig& data_cfg,
               const config::SimulationConfig& sim_cfg,
               const std::vector<config::InstrumentConfig>& instruments);

    // Checks that every required trades + orderbook file exists for the full
    // date range.  Throws std::runtime_error listing all missing files unless
    // allow_partial_data is set, in which case missing ranges are logged and
    // clipped.
    void validate();

    // Returns the next event in timestamp order across all instruments.
    // Returns nullopt when all data for the configured window is exhausted.
    std::optional<MarketEvent> next();

    bool exhausted() const { return heap_.empty() && !load_next_day(); }

private:
    // One per instrument — tracks position within the current day's events
    // and advances to the next day when exhausted.
    struct InstrumentReader {
        config::InstrumentConfig instrument;
        std::chrono::sys_days current_day;
        std::chrono::sys_days end_day;
        std::vector<MarketEvent> day_events;  // all events for current_day, sorted
        std::size_t pos{0};

        bool has_next() const { return pos < day_events.size(); }
        const MarketEvent& peek() const { return day_events[pos]; }
        MarketEvent consume() { return std::move(day_events[pos++]); }
        bool advance();  // move to next day; returns false if past end_day
    };

    // Loads the next available day across all readers into the heap.
    // Returns false if all readers are exhausted.
    bool load_next_day() const;

    std::string trades_path(const std::string& exchange, const std::string& symbol, std::chrono::sys_days day) const;
    std::string orderbook_path(const std::string& exchange, const std::string& symbol, std::chrono::sys_days day) const;

    void load_day(InstrumentReader& reader);

    std::vector<MarketEvent> read_trades(const std::string& path,
                                         const std::string& exchange,
                                         const std::string& symbol) const;
    std::vector<MarketEvent> read_orderbook(const std::string& path,
                                            const std::string& exchange,
                                            const std::string& symbol) const;

    std::string local_cache_;
    bool allow_partial_data_;
    std::chrono::sys_days start_day_;
    std::chrono::sys_days end_day_;
    std::vector<InstrumentReader> readers_;

    // Min-heap: (timestamp_ns, reader_index)
    using HeapEntry = std::pair<uint64_t, std::size_t>;
    mutable std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> heap_;
};

}  // namespace bpt::backtester::data
