#include "order_gateway/risk/pnl_tracker.h"

#include <algorithm>

namespace bpt::order_gateway::risk {

namespace {
uint64_t make_key(bpt::messages::ExchangeId::Value exchange, uint64_t instrument_id) {
    return (static_cast<uint64_t>(exchange) << 56) | (instrument_id & 0x00ffffffffffffffULL);
}
}  // namespace

uint64_t PnlTracker::utc_day_of(uint64_t ns) {
    // ns since Unix epoch → day index (floor div by 86400 seconds × 1e9 ns).
    constexpr uint64_t ns_per_day = 86400ULL * 1'000'000'000ULL;
    return ns / ns_per_day;
}

void PnlTracker::roll_day_if_needed(uint64_t now_ns) {
    const uint64_t day = utc_day_of(now_ns);
    if (day != current_utc_day_) {
        current_utc_day_ = day;
        daily_pnl_usd_ = 0.0;
    }
}

void PnlTracker::on_fill(bpt::messages::ExchangeId::Value exchange,
                         uint64_t instrument_id,
                         bpt::messages::OrderSide::Value side,
                         int64_t price_e8,
                         uint64_t filled_qty_e8,
                         uint64_t fill_ts_ns) {
    roll_day_if_needed(fill_ts_ns);

    const double price = static_cast<double>(price_e8) / kScale;
    const double qty = static_cast<double>(filled_qty_e8) / kScale;

    Position& pos = positions_[make_key(exchange, instrument_id)];

    using bpt::messages::OrderSide;
    if (side == OrderSide::BUY) {
        if (pos.net_qty_e8 >= 0) {
            // Adding to long or flat — update weighted average entry.
            const double prev_qty = static_cast<double>(pos.net_qty_e8) / kScale;
            const double new_qty = prev_qty + qty;
            pos.avg_price_usd = (new_qty > 0.0) ? (pos.avg_price_usd * prev_qty + price * qty) / new_qty : 0.0;
        } else {
            // Closing or flipping a short.
            const double short_qty = -static_cast<double>(pos.net_qty_e8) / kScale;
            const double closed = std::min(qty, short_qty);
            const double pnl = closed * (pos.avg_price_usd - price);
            daily_pnl_usd_ += pnl;
            session_pnl_usd_ += pnl;
            if (qty > short_qty) {
                // Flipped through flat to long — new avg_entry is the fill price.
                pos.avg_price_usd = price;
            }
            // If partial close, avg remains (same short entry).
        }
        pos.net_qty_e8 += static_cast<int64_t>(filled_qty_e8);

    } else {  // SELL
        if (pos.net_qty_e8 <= 0) {
            // Adding to short or flat — update weighted average entry.
            const double prev_qty = -static_cast<double>(pos.net_qty_e8) / kScale;
            const double new_qty = prev_qty + qty;
            pos.avg_price_usd = (new_qty > 0.0) ? (pos.avg_price_usd * prev_qty + price * qty) / new_qty : 0.0;
        } else {
            // Closing or flipping a long.
            const double long_qty = static_cast<double>(pos.net_qty_e8) / kScale;
            const double closed = std::min(qty, long_qty);
            const double pnl = closed * (price - pos.avg_price_usd);
            daily_pnl_usd_ += pnl;
            session_pnl_usd_ += pnl;
            if (qty > long_qty) {
                pos.avg_price_usd = price;
            }
        }
        pos.net_qty_e8 -= static_cast<int64_t>(filled_qty_e8);
    }

    // Prune flat positions. Realised P&L has already been accumulated
    // into daily_pnl_usd_ / session_pnl_usd_; the entry holds no further
    // state once net_qty_e8 == 0. net_qty_e8() returns 0 for unknown
    // keys, so downstream queries are unchanged — this just stops the
    // map from growing unboundedly as instruments are traded to flat.
    if (pos.net_qty_e8 == 0)
        positions_.erase(make_key(exchange, instrument_id));
}

double PnlTracker::daily_realized_pnl_usd(uint64_t now_ns) {
    roll_day_if_needed(now_ns);
    return daily_pnl_usd_;
}

int64_t PnlTracker::net_qty_e8(bpt::messages::ExchangeId::Value exchange, uint64_t instrument_id) const noexcept {
    const auto it = positions_.find(make_key(exchange, instrument_id));
    return it == positions_.end() ? 0 : it->second.net_qty_e8;
}

}  // namespace bpt::order_gateway::risk
