#pragma once

#include "backtester/data/market_event.h"
#include "backtester/matching/open_order.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::backtester::matching {

// Simulates a per-exchange order book and fills incoming orders against it.
//
// Thread safety:
//   on_market_event() is called from the ClockMaster thread.
//   submit_order() and cancel_order() are called from the BinanceOrderServer's
//   io_context thread.  All public methods are protected by an internal mutex.
//   The fill callback is invoked *outside* the mutex to prevent deadlocks.
class MatchingEngine {
public:
    using FillCallback = std::function<void(FillReport)>;

    void set_fill_callback(FillCallback cb);

    // Called from ClockMaster on every tick.
    // Updates the L2 book snapshot and attempts to fill pending limit orders.
    void on_market_event(const data::MarketEvent& event);

    // Called from the order WS/REST server when Heimdall submits an order.
    // MARKET orders fill synchronously against the current book; the fill
    // callback fires before this returns.
    // LIMIT orders are queued and filled on future book updates.
    // Returns the order with updated filled_qty.
    OpenOrder submit_order(OpenOrder order);

    // Returns true if the order was found and cancelled.
    bool cancel_order(const std::string& exchange, const std::string& symbol, const std::string& order_id);

private:
    static std::string key(const std::string& exchange, const std::string& symbol);

    // Fills a MARKET order against the book; appends fill reports to out.
    // Caller must hold mutex_.
    void fill_market(OpenOrder& order, const data::OrderBookRecord& book, std::vector<FillReport>& out);

    // Scans pending limit orders for key and fills crossing ones; appends to out.
    // Caller must hold mutex_.
    void fill_crossing_limits(const std::string& book_key, std::vector<FillReport>& out);

    std::mutex mutex_;
    std::unordered_map<std::string, data::OrderBookRecord> books_;
    std::unordered_map<std::string, std::vector<OpenOrder>> pending_;
    uint64_t current_ts_{0};
    FillCallback fill_cb_;
};

}  // namespace bpt::backtester::matching
