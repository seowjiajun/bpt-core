#pragma once

/// \file
/// \brief MatchingEngine — simulated per-exchange order book and fill engine.

#include "backtester/data/market_event.h"
#include "backtester/latency/latency_model.h"
#include "backtester/matching/open_order.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::backtester::matching {

/// \brief Simulates a per-exchange order book and fills incoming orders against it.
///
/// Thread safety:
///   on_market_event() is called from the ClockMaster thread.
///   submit_order() and cancel_order() are called from the BinanceOrderServer's
///   io_context thread.  All public methods are protected by an internal mutex.
///   The fill callback is invoked *outside* the mutex to prevent deadlocks.
class MatchingEngine {
public:
    using FillCallback = std::function<void(FillReport)>;

    void set_fill_callback(FillCallback cb);

    /// \brief Optional latency model.
    ///
    /// When null (default), submit_to_match and match_to_report delays are
    /// zero — pre-Phase-3 behaviour. When set, every submitted order's
    /// match is deferred until current_ts_ ≥ submitted_ts +
    /// draw(SUBMIT_TO_MATCH); every produced fill's delivery to fill_cb_ is
    /// deferred until current_ts_ ≥ match_ts + draw(MATCH_TO_REPORT).
    void set_latency_model(latency::LatencyModel* model);

    /// \brief Called from ClockMaster on every tick.
    ///
    /// Order: drain pending submits whose scheduled_match_ts ≤ event.ts,
    /// then update book / run trade or crossing fills, then drain pending
    /// fill deliveries whose scheduled_report_ts ≤ current_ts_.
    void on_market_event(const data::MarketEvent& event);

    /// \brief Called from the order WS/REST server when OrderGateway submits an order.
    ///
    /// POST_ONLY orders that would cross the current book are synchronously
    /// rejected (real exchanges return this in the ack); everything else
    /// returns ACCEPTED immediately and the actual match is deferred per the
    /// latency model. Fills come later via fill_cb_.
    OpenOrder submit_order(OpenOrder order);

    /// \brief Cancel an order by venue/symbol/id.
    ///
    /// Scans both pending_submits_ (orders waiting on submit_to_match) and
    /// pending_ (resting limits awaiting fills).
    /// \return True if the order was found and cancelled.
    bool cancel_order(const std::string& exchange, const std::string& symbol, const std::string& order_id);

private:
    static std::string key(const std::string& exchange, const std::string& symbol);

    /// \brief Fills a MARKET order against the book; appends fill reports to out.
    ///
    /// Caller must hold mutex_.
    void fill_market(OpenOrder& order, const data::OrderBookRecord& book, std::vector<FillReport>& out);

    /// \brief Fills `order` aggressively against `book`, walking levels until
    ///        either the order is fully filled or the next level's price is
    ///        worse than `price_limit` (BUY: ask > price_limit, SELL: bid <
    ///        price_limit).
    ///
    /// Used by both fill_market (no cap, +/- infinity) and the crossing-LIMIT
    /// path at submit time (cap = limit price). Emitted FillReports are
    /// tagged with `report_type` and TAKER. Caller must hold mutex_.
    void fill_book_until(OpenOrder& order,
                         const data::OrderBookRecord& book,
                         double price_limit,
                         OrderType report_type,
                         std::vector<FillReport>& out);

    /// \brief Scans pending limit orders for key and fills crossing ones; appends to out.
    ///
    /// Caller must hold mutex_.
    void fill_crossing_limits(const std::string& book_key, std::vector<FillReport>& out);

    /// \brief Drains queue_ahead from resting LIMITs at the trade price, then
    ///        fills the residual against us in FIFO order.
    ///
    /// Caller must hold mutex_.
    /// Counter-side semantics:
    ///   trade.side == SELL → taker sold → consumed bid queue → our resting BUYs may fill
    ///   trade.side == BUY  → taker bought → consumed ask queue → our resting SELLs may fill
    void fill_against_trade(const data::TradeRecord& trade, std::vector<FillReport>& out);

    /// \brief Look up resting volume at a given price level on a given side.
    ///
    /// Returns 0.0 if the price isn't found in the L5 snapshot. Used to
    /// seed queue_ahead at submit_order time.
    static double book_qty_at_price(const data::OrderBookRecord& book, OrderSide side, double price);

    /// \brief Pending submit awaiting its scheduled match time.
    ///
    /// Synchronously enqueued at submit_order, drained at on_market_event
    /// when current_ts_ catches up to scheduled_match_ts. The order's
    /// submitted_ts (set in submit_order) is preserved; queue_ahead is
    /// seeded at drain time against the book at scheduled_match_ts, which
    /// is what makes Option-A latency capture stale-quote adverse selection.
    struct PendingSubmit {
        OpenOrder order;
        uint64_t scheduled_match_ts;
    };
    struct PendingFill {
        FillReport fill;
        uint64_t scheduled_report_ts;
    };

    /// \brief Runs the actual matching logic for a single order at its scheduled
    ///        match time.
    ///
    /// Equivalent to the pre-Phase-3 body of submit_order (minus the
    /// synchronous POST_ONLY-cross check, which fires earlier). Caller must
    /// hold mutex_.
    void process_pending_submit(PendingSubmit& ps, std::vector<FillReport>& out);

    /// \brief Pops all pending submits with scheduled_match_ts ≤ upto_ts, in
    ///        chronological order, processing each through the match logic.
    ///
    /// Produced fills are scheduled for delivery via schedule_fill. Caller
    /// must hold mutex_.
    void drain_pending_submits(uint64_t upto_ts);

    /// \brief Schedules a fill for delivery at fill.simulation_ts + match_to_report.
    ///
    /// Caller must hold mutex_.
    void schedule_fill(FillReport fill);

    /// \brief Pops all pending fills with scheduled_report_ts ≤ upto_ts and
    ///        appends them to out.
    ///
    /// Caller must hold mutex_; fill_cb_ fires outside the lock by the caller.
    void drain_pending_fills(uint64_t upto_ts, std::vector<FillReport>& out);

    /// \brief Phase 5: when a book update arrives, attribute the volume drop at
    ///        each price level to (cancels) = (Δ size) − (trade volume since last
    ///        book update at that level).
    ///
    /// Decrement queue_ahead on resting orders proportionally to the cancel
    /// share, under the uniform-cancel approximation (each unit ahead has
    /// equal probability of being a cancel). Caller must hold mutex_; called
    /// between drain_pending_submits and the books_[key] write.
    void apply_queue_regen(const std::string& book_key, const data::OrderBookRecord& new_book);

    /// \brief Discrete key for a price level.
    ///
    /// Backed by round(price * 1e9) so we dodge double-keyed unordered_map
    /// collision risk while keeping lookups exact for venue-tick-aligned prices.
    static int64_t price_key(double price);

    std::mutex mutex_;
    std::unordered_map<std::string, data::OrderBookRecord> books_;
    std::unordered_map<std::string, std::vector<OpenOrder>> pending_;
    std::vector<PendingSubmit> pending_submits_;  ///< sorted by scheduled_match_ts when drained.
    std::vector<PendingFill> pending_fills_;      ///< sorted by scheduled_report_ts when drained.
    /// Trade volume per (instrument, price) since the most recent book
    /// snapshot. Drained on each book update after queue-regen attribution.
    std::unordered_map<std::string, std::unordered_map<int64_t, double>> traded_since_book_;
    uint64_t current_ts_{0};
    FillCallback fill_cb_;
    latency::LatencyModel* latency_{nullptr};
};

}  // namespace bpt::backtester::matching
