#include "features/queue.h"

#include <algorithm>

namespace bpt::features {

using bpt::messages::OrderSide;

void QueueTracker::track(uint64_t order_id,
                         OrderSide::Value side,
                         double price,
                         double our_qty,
                         uint64_t ts_ns,
                         const OrderBookState& book) {
    double ahead_better = 0.0;
    double at_level = 0.0;
    if (side == OrderSide::BUY) {
        ahead_better = book.bid_vol_above(price);
        at_level = book.size_at_bid(price);
    } else if (side == OrderSide::SELL) {
        ahead_better = book.ask_vol_below(price);
        at_level = book.size_at_ask(price);
    } else {
        // Unknown side — don't track.
        return;
    }

    Entry e;
    e.side = side;
    e.price = price;
    e.our_qty = our_qty;
    e.queue_ahead = ahead_better + at_level;  // see header: conservative
    e.placed_ns = ts_ns;
    entries_[order_id] = e;
}

void QueueTracker::on_fill(uint64_t order_id, double filled_qty) {
    auto it = entries_.find(order_id);
    if (it == entries_.end())
        return;
    it->second.our_qty -= filled_qty;
    if (it->second.our_qty <= 0.0)
        entries_.erase(it);
}

void QueueTracker::on_cancel(uint64_t order_id) {
    entries_.erase(order_id);
}

void QueueTracker::on_trade(OrderSide::Value aggressor, double trade_price, double trade_qty, uint64_t /*ts_ns*/) {
    // Aggressive BUY consumes ask-side passives; aggressive SELL consumes
    // bid-side passives.
    const OrderSide::Value passive_side = (aggressor == OrderSide::BUY) ? OrderSide::SELL : OrderSide::BUY;

    for (auto& [_, entry] : entries_) {
        if (entry.side != passive_side)
            continue;
        if (entry.price != trade_price)
            continue;
        entry.queue_ahead = std::max(0.0, entry.queue_ahead - trade_qty);
    }
}

const QueueTracker::Entry* QueueTracker::lookup(uint64_t order_id) const {
    auto it = entries_.find(order_id);
    return it == entries_.end() ? nullptr : &it->second;
}

double QueueTracker::fill_probability(uint64_t order_id) const {
    const Entry* e = lookup(order_id);
    if (e == nullptr || e->our_qty <= 0.0)
        return 0.0;
    const double denom = e->our_qty + e->queue_ahead;
    if (denom <= 0.0)
        return 1.0;
    return e->our_qty / denom;
}

}  // namespace bpt::features
