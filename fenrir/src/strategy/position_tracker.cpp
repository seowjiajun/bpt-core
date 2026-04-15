#include "fenrir/strategy/position_tracker.h"

#include <algorithm>
#include <cmath>

namespace fenrir::strategy {

static constexpr double kPriceScale = 1e8;
static constexpr double kQtyScale = 1e8;

void PositionTracker::on_fill(uint64_t instrument_id,
                              bifrost::protocol::ExchangeId::Value exchange_id,
                              bifrost::protocol::OrderSide::Value side,
                              uint64_t filled_qty,
                              int64_t fill_price) {
    const uint64_t k = key(instrument_id, exchange_id);
    Position& pos = positions_[k];
    const double price = static_cast<double>(fill_price) / kPriceScale;
    const double qty = static_cast<double>(filled_qty) / kQtyScale;

    if (side == bifrost::protocol::OrderSide::BUY) {
        if (pos.net_qty >= 0) {
            // Adding to long: update weighted average entry price.
            const double prev_qty = static_cast<double>(pos.net_qty) / kQtyScale;
            const double new_qty = prev_qty + qty;
            pos.avg_price = (pos.avg_price * prev_qty + price * qty) / new_qty;
        } else {
            // Closing short (full or partial).
            const double short_qty = -static_cast<double>(pos.net_qty) / kQtyScale;
            const double closed = std::min(qty, short_qty);
            pos.realized_pnl += closed * (pos.avg_price - price);
            if (qty >= short_qty) {
                // Fully closed (or flipped to long).
                pos.avg_price = (qty > short_qty) ? price : 0.0;
            }
            // If partial close, avg_price stays the same (still short at same entry).
        }
        pos.net_qty += static_cast<int64_t>(filled_qty);

    } else {  // SELL
        if (pos.net_qty <= 0) {
            // Adding to short: update weighted average entry price.
            const double prev_qty = -static_cast<double>(pos.net_qty) / kQtyScale;
            const double new_qty = prev_qty + qty;
            pos.avg_price = (pos.avg_price * prev_qty + price * qty) / new_qty;
        } else {
            // Closing long (full or partial).
            const double long_qty = static_cast<double>(pos.net_qty) / kQtyScale;
            const double closed = std::min(qty, long_qty);
            pos.realized_pnl += closed * (price - pos.avg_price);
            if (qty >= long_qty) {
                // Fully closed (or flipped to short).
                pos.avg_price = (qty > long_qty) ? price : 0.0;
            }
        }
        pos.net_qty -= static_cast<int64_t>(filled_qty);
    }
}

std::optional<PositionTracker::Position> PositionTracker::get(uint64_t instrument_id,
                                                              bifrost::protocol::ExchangeId::Value exchange_id) const {
    auto it = positions_.find(key(instrument_id, exchange_id));
    if (it == positions_.end())
        return std::nullopt;
    return it->second;
}

int64_t PositionTracker::net_qty(uint64_t instrument_id, bifrost::protocol::ExchangeId::Value exchange_id) const {
    auto it = positions_.find(key(instrument_id, exchange_id));
    if (it == positions_.end())
        return 0;
    return it->second.net_qty;
}

void PositionTracker::clear(uint64_t instrument_id, bifrost::protocol::ExchangeId::Value exchange_id) {
    positions_.erase(key(instrument_id, exchange_id));
}

void PositionTracker::clear_all() {
    positions_.clear();
}

}  // namespace fenrir::strategy
