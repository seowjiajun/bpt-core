#pragma once

/// \file
/// \brief MatchingEngine — backtest simulator with a synthetic L3 order book.

#include "backtester/data/market_event.h"
#include "backtester/latency/latency_model.h"
#include "backtester/matching/open_order.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::backtester::matching {

/// \brief Simulates a per-exchange order book and fills incoming orders against it.
///
/// The engine takes the role of a venue's matching engine in a backtest
/// run. It accepts orders from the simulated OrderGateway, consumes a
/// stream of replayed market events (L2 snapshots and trade prints), and
/// emits FillReports to a caller-supplied callback. Internally it
/// maintains a per-(book, side, price) synthetic L3 deque reconstructed
/// from the L2 stream — orders rest in their queue position and fill
/// FIFO as trade prints walk the front of the deque.
///
/// Thread safety: on_market_event() is called from the ClockMaster
/// thread; submit_order() and cancel_order() are called from the order
/// server's io_context thread. All public methods take an internal
/// mutex. The fill callback is invoked outside the mutex to prevent
/// deadlocks if the callback re-enters the engine.
///
/// \par Example usage
/// \code
///   MatchingEngine eng;
///   eng.set_fill_callback([](FillReport r) {
///       // route fill to the strategy / results collector
///   });
///   eng.set_latency_model(&my_latency_model);   // optional
///
///   // Drive from ClockMaster on every replayed event
///   eng.on_market_event(event);
///
///   // Submit / cancel orders from the simulated OrderGateway
///   OpenOrder ack = eng.submit_order(order);
///   if (ack.rejected) { /* venue rejected at submit time */ }
///   eng.cancel_order(exchange, symbol, order_id);
/// \endcode
///
/// \see OpenOrder, FillReport, latency::LatencyModel
class MatchingEngine {
public:
    /// \brief Callback type invoked when the engine produces a fill report.
    ///
    /// Called from on_market_event() or submit_order() outside the engine's
    /// mutex. Implementations may safely call back into the engine.
    using FillCallback = std::function<void(FillReport)>;

    /// \brief Installs the callback that receives every produced FillReport.
    /// \param cb Callable invoked with each FillReport. Replaces any prior
    ///           callback. Pass an empty function to disable.
    /// \see FillReport
    void set_fill_callback(FillCallback cb);

    /// \brief Installs an optional latency model controlling fill timing.
    ///
    /// When null (default), submit-to-match and match-to-report delays are
    /// zero. When set, every submitted order's match is deferred until the
    /// engine's current simulated timestamp reaches submitted_ts +
    /// draw(SUBMIT_TO_MATCH); every produced fill is queued until
    /// match_ts + draw(MATCH_TO_REPORT) before being handed to the
    /// fill callback.
    ///
    /// \param model Non-owning pointer to a model. The model must outlive
    ///              the engine. nullptr restores zero-latency behaviour.
    /// \see latency::LatencyModel
    void set_latency_model(latency::LatencyModel* model);

    /// \brief Processes a single replayed market event (L2 book or trade print).
    ///
    /// Drives the engine's simulated clock forward. On every call:
    ///   1. Pending submits whose scheduled_match_ts has arrived are
    ///      executed (matched against the current book).
    ///   2. The book / slot deque is updated:
    ///      - ORDER_BOOK event: reconcile slot deques against the new L2
    ///        snapshot. Crossing-LIMIT orders at the new touch may fill.
    ///      - TRADE event: walk the slot deque at the trade price and
    ///        consume slots FIFO.
    ///   3. Pending fills whose scheduled_report_ts has arrived are
    ///      delivered to the fill callback.
    ///
    /// \param event An ORDER_BOOK or TRADE event from the replay stream.
    void on_market_event(const data::MarketEvent& event);

    /// \brief Submits an order and returns the acknowledgement.
    ///
    /// POST_ONLY orders that would cross the current book are rejected
    /// synchronously — real venues return this in the ack frame (HL Alo,
    /// OKX post_only, Binance LIMIT_MAKER). All other order types accept
    /// immediately and their match is deferred per the latency model;
    /// fills are delivered later via the fill callback.
    ///
    /// \param order An OpenOrder populated by the caller (order_id,
    ///              client_order_id, exchange, symbol, type, side, price,
    ///              quantity must be set; the engine sets submitted_ts).
    /// \return A copy of `order` with submitted_ts set and `rejected` true
    ///         iff the engine refused the order synchronously.
    /// \see OpenOrder, cancel_order
    OpenOrder submit_order(OpenOrder order);

    /// \brief Cancels a resting or yet-to-match order by venue/symbol/id.
    ///
    /// Searches both the pending-submit queue (orders waiting on
    /// submit-to-match latency) and the resting-LIMIT pool (orders that
    /// have a queue position).
    ///
    /// \param exchange Wire-format exchange name.
    /// \param symbol Venue-native symbol.
    /// \param order_id Engine-unique order id matching the original submit.
    /// \return True if the order was found and removed; false otherwise.
    /// \see submit_order
    bool cancel_order(const std::string& exchange, const std::string& symbol, const std::string& order_id);

private:
    static std::string key(const std::string& exchange, const std::string& symbol);

    void fill_market(OpenOrder& order, const data::OrderBookRecord& book, std::vector<FillReport>& out);
    void fill_book_until(OpenOrder& order,
                         const data::OrderBookRecord& book,
                         double price_limit,
                         OrderType report_type,
                         std::vector<FillReport>& out);
    void fill_crossing_limits(const std::string& book_key, std::vector<FillReport>& out);
    void fill_against_trade(const data::TradeRecord& trade, std::vector<FillReport>& out);

    static double book_qty_at_price(const data::OrderBookRecord& book, OrderSide side, double price);

    struct PendingSubmit {
        OpenOrder order;
        uint64_t scheduled_match_ts;
    };
    struct PendingFill {
        FillReport fill;
        uint64_t scheduled_report_ts;
    };

    void process_pending_submit(PendingSubmit& ps, std::vector<FillReport>& out);
    void drain_pending_submits(uint64_t upto_ts);
    void schedule_fill(FillReport fill);
    void drain_pending_fills(uint64_t upto_ts, std::vector<FillReport>& out);
    void reconcile_l2_snapshot(const std::string& book_key, const data::OrderBookRecord& new_book);

    static int64_t price_key(double price);

    /// One unit in a price-level FIFO queue. is_ours==true means the slot
    /// is one of our OpenOrders (linked by our_order_id); is_ours==false
    /// means an inferred venue slot whose qty came from an observed L2
    /// add.
    struct Slot {
        double qty{0.0};
        bool is_ours{false};
        std::string our_order_id;
        uint64_t inferred_ts{0};
    };
    struct PriceQueues {
        std::unordered_map<int64_t, std::deque<Slot>> bid;
        std::unordered_map<int64_t, std::deque<Slot>> ask;
    };
    std::deque<Slot>& level_queue(const std::string& book_key, OrderSide side, double price);
    std::deque<Slot>* level_queue_if_exists(const std::string& book_key, OrderSide side, double price);
    static void distribute_cancels(std::deque<Slot>& queue, double cancels);

    std::mutex mutex_;
    std::unordered_map<std::string, data::OrderBookRecord> books_;
    std::unordered_map<std::string, std::vector<OpenOrder>> pending_;
    std::vector<PendingSubmit> pending_submits_;
    std::vector<PendingFill> pending_fills_;
    std::unordered_map<std::string, PriceQueues> slot_queues_;
    uint64_t current_ts_{0};
    FillCallback fill_cb_;
    latency::LatencyModel* latency_{nullptr};
};

}  // namespace bpt::backtester::matching
