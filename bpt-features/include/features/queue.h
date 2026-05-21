#pragma once

#include "bpt_common/book/order_book_state.h"

#include <messages/OrderSide.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace bpt::features {

using bpt::common::book::OrderBookState;

// Per-resting-order queue-position tracker.
//
// When we place an order at price P on a given side, everything already at
// prices strictly better than P plus whatever's at P ahead of us has to be
// filled (or cancelled) before we do. This class estimates that
// "queue_ahead" quantity and decrements it as trades print at our price.
//
// Usage pattern:
//   tracker.track(order_id, BID, px, qty, ts_ns, book_state);
//   ... as trades arrive on the bid side at px, tracker.on_trade(...)
//       decrements queue_ahead for that entry
//   ... on our own fill/cancel, tracker.on_fill / on_cancel
//   ... fill probability signal = our_qty / (our_qty + queue_ahead) or
//       similar functional form; consumed by Phase 4 quoting logic.
//
// Not thread-safe — single-writer from the strategy poll thread.
class QueueTracker {
public:
    struct Entry {
        bpt::messages::OrderSide::Value side{bpt::messages::OrderSide::NULL_VALUE};
        double price{0.0};
        double our_qty{0.0};      // remaining unfilled size of our order
        double queue_ahead{0.0};  // estimated volume at-or-better than `price` that sits in front of us
        uint64_t placed_ns{0};
    };

    // Register a newly acked resting order. Snapshots queue_ahead from the
    // current book state — this is the most important single data point
    // for the strategy, so callers should invoke this as close to the
    // exchange ack as possible.
    //
    // Conservative assumption: the book snapshot was taken BEFORE the
    // exchange added our resting order, so the entirety of size_at(price)
    // is considered ahead of us. That slightly underestimates fill
    // probability (worst-case), which is the safer direction for a
    // market-making strategy — we'd rather quote more aggressively than
    // get hit unexpectedly.
    void track(uint64_t order_id,
               bpt::messages::OrderSide::Value side,
               double price,
               double our_qty,
               uint64_t ts_ns,
               const OrderBookState& book);

    // Partial or full fill on our order. Decrements our_qty; drops the
    // entry once our_qty reaches 0.
    void on_fill(uint64_t order_id, double filled_qty);

    // Order cancelled or otherwise terminated — drop the entry.
    void on_cancel(uint64_t order_id);

    // A public-market trade printed. For every tracked entry whose price
    // matches the trade price AND whose side is the passive side of the
    // trade (aggressor opposite), decrement queue_ahead by trade qty.
    // Clamped at 0.
    //
    // Aggressor semantics: an aggressive BUY lifts offers (ask-side
    // passive fills). An aggressive SELL hits bids (bid-side passive
    // fills). `aggressor` is the side of the incoming taker.
    void on_trade(bpt::messages::OrderSide::Value aggressor, double trade_price, double trade_qty, uint64_t ts_ns);

    // Lookup. Returns nullptr if not tracked.
    const Entry* lookup(uint64_t order_id) const;

    // Fill-probability proxy in [0, 1]. High when queue_ahead is small
    // relative to our size; low when we're buried in the queue.
    //   p = our_qty / (our_qty + queue_ahead)
    // Returns 0 if order_id is unknown. This is a simplistic signal;
    // Phase 4 can refine using the trade-arrival-rate estimate (κ) that
    // AS already maintains.
    double fill_probability(uint64_t order_id) const;

    size_t size() const { return entries_.size(); }

private:
    std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace bpt::features
