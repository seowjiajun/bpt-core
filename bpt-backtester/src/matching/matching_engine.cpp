#include "backtester/matching/matching_engine.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"

#include <algorithm>
#include <format>
#include <yggdrasil/logging.h>

namespace bpt::backtester::matching {

// ── Internal helpers ──────────────────────────────────────────────────────────

std::string MatchingEngine::key(const std::string& exchange, const std::string& symbol) {
    return exchange + ':' + symbol;
}

void MatchingEngine::set_fill_callback(FillCallback cb) {
    std::lock_guard lock(mutex_);
    fill_cb_ = std::move(cb);
}

// ── Market event ──────────────────────────────────────────────────────────────

void MatchingEngine::on_market_event(const data::MarketEvent& event) {
    std::vector<FillReport> fills;

    {
        std::lock_guard lock(mutex_);
        if (event.type == data::MarketEvent::Type::ORDER_BOOK) {
            const auto& ob = std::get<data::OrderBookRecord>(event.payload);
            current_ts_ = ob.timestamp_ns;
            books_[key(ob.exchange, ob.symbol)] = ob;
            fill_crossing_limits(key(ob.exchange, ob.symbol), fills);
        } else {
            const auto& t = std::get<data::TradeRecord>(event.payload);
            current_ts_ = t.timestamp_ns;
            // Trades carry price info but don't give us a full book snapshot;
            // limit matching only runs on order-book updates.
        }
    }

    for (auto& fill : fills)
        if (fill_cb_)
            fill_cb_(fill);
}

// ── Order submission ──────────────────────────────────────────────────────────

OpenOrder MatchingEngine::submit_order(OpenOrder order) {
    std::vector<FillReport> fills;

    {
        std::lock_guard lock(mutex_);
        order.submitted_ts = current_ts_;

        if (order.type == OrderType::MARKET) {
            auto it = books_.find(key(order.exchange, order.symbol));
            if (it != books_.end()) {
                fill_market(order, it->second, fills);
            } else {
                ygg::log::warn("[MatchingEngine] No book for {}/{} — market order unfilled",
                               order.exchange,
                               order.symbol);
            }
        } else {
            pending_[key(order.exchange, order.symbol)].push_back(order);
            ygg::log::debug("[MatchingEngine] Queued LIMIT {} {} {} @ {}",
                            order.symbol,
                            (order.side == OrderSide::BUY ? "BUY" : "SELL"),
                            order.quantity,
                            order.price);
        }
    }

    for (auto& fill : fills)
        if (fill_cb_)
            fill_cb_(fill);

    return order;
}

bool MatchingEngine::cancel_order(const std::string& exchange, const std::string& symbol, const std::string& order_id) {
    std::lock_guard lock(mutex_);
    auto it = pending_.find(key(exchange, symbol));
    if (it == pending_.end())
        return false;

    auto& v = it->second;
    auto pos = std::find_if(v.begin(), v.end(), [&](const OpenOrder& o) { return o.order_id == order_id; });
    if (pos == v.end())
        return false;
    v.erase(pos);
    return true;
}

// ── Matching logic ────────────────────────────────────────────────────────────

void MatchingEngine::fill_market(OpenOrder& order, const data::OrderBookRecord& book, std::vector<FillReport>& out) {
    double remaining = order.quantity - order.filled_qty;

    for (int lvl = 0; lvl < data::kOrderBookDepth && remaining > 1e-12; ++lvl) {
        double level_px, level_sz;
        if (order.side == OrderSide::BUY) {
            level_px = book.ask_px[lvl];
            level_sz = book.ask_sz[lvl];
        } else {
            level_px = book.bid_px[lvl];
            level_sz = book.bid_sz[lvl];
        }

        if (level_px <= 0.0 || level_sz <= 0.0)
            break;

        double fill_qty = std::min(remaining, level_sz);
        order.filled_qty += fill_qty;
        remaining -= fill_qty;

        FillReport r;
        r.order_id = order.order_id;
        r.client_order_id = order.client_order_id;
        r.exchange = order.exchange;
        r.symbol = order.symbol;
        r.order_type = OrderType::MARKET;
        r.side = order.side;
        r.original_qty = order.quantity;
        r.order_price = 0.0;
        r.last_fill_qty = fill_qty;
        r.last_fill_price = level_px;
        r.cumulative_fill_qty = order.filled_qty;
        r.is_fully_filled = (remaining <= 1e-12);
        r.simulation_ts = current_ts_;
        out.push_back(r);
    }

    if (remaining > 1e-12) {
        ygg::log::warn("[MatchingEngine] Market order {} partially filled: {}/{} — book too thin",
                       order.order_id,
                       order.filled_qty,
                       order.quantity);
    }
}

void MatchingEngine::fill_crossing_limits(const std::string& book_key, std::vector<FillReport>& out) {
    auto bit = books_.find(book_key);
    if (bit == books_.end())
        return;
    const auto& book = bit->second;

    if (book.bid_px[0] <= 0.0 && book.ask_px[0] <= 0.0)
        return;

    auto pit = pending_.find(book_key);
    if (pit == pending_.end())
        return;
    auto& orders = pit->second;

    orders.erase(std::remove_if(orders.begin(),
                                orders.end(),
                                [&](OpenOrder& order) {
                                    if (order.type != OrderType::LIMIT)
                                        return false;

                                    double fill_px = 0.0;
                                    if (order.side == OrderSide::BUY) {
                                        // Fill if best ask has come down to or below limit price.
                                        if (book.ask_px[0] > 0.0 && book.ask_px[0] <= order.price)
                                            fill_px = book.ask_px[0];
                                    } else {
                                        // Fill if best bid has risen to or above limit price.
                                        if (book.bid_px[0] > 0.0 && book.bid_px[0] >= order.price)
                                            fill_px = book.bid_px[0];
                                    }

                                    if (fill_px <= 0.0)
                                        return false;

                                    double fill_qty = order.quantity - order.filled_qty;
                                    order.filled_qty = order.quantity;

                                    FillReport r;
                                    r.order_id = order.order_id;
                                    r.client_order_id = order.client_order_id;
                                    r.exchange = book.exchange;
                                    r.symbol = book.symbol;
                                    r.order_type = OrderType::LIMIT;
                                    r.side = order.side;
                                    r.original_qty = order.quantity;
                                    r.order_price = order.price;
                                    r.last_fill_qty = fill_qty;
                                    r.last_fill_price = fill_px;
                                    r.cumulative_fill_qty = order.quantity;
                                    r.is_fully_filled = true;
                                    r.simulation_ts = current_ts_;
                                    out.push_back(r);
                                    return true;  // remove from pending
                                }),
                 orders.end());
}

}  // namespace bpt::backtester::matching
