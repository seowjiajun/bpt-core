#include "backtester/matching/matching_engine.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <bpt_common/logging.h>

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
            // Backstop path for orders whose queue_ahead couldn't be
            // seeded (deeper than L5, or pre-book-snapshot). These
            // still fill on book cross — over-optimistic but rare.
            fill_crossing_limits(key(ob.exchange, ob.symbol), fills);
        } else {
            const auto& t = std::get<data::TradeRecord>(event.payload);
            current_ts_ = t.timestamp_ns;
            // Primary fill path: trade prints drain queue_ahead first,
            // then fill us. This is the queue-aware model. The legacy
            // book-cross path above remains as a backstop only.
            fill_against_trade(t, fills);
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
                bpt::common::log::warn("[MatchingEngine] No book for {}/{} — market order unfilled",
                               order.exchange,
                               order.symbol);
            }
        } else {
            auto it = books_.find(key(order.exchange, order.symbol));
            if (it != books_.end()) {
                const auto& book = it->second;

                const bool crosses =
                    (order.side == OrderSide::BUY  && book.ask_px[0] > 0.0 && order.price >= book.ask_px[0] - 1e-9) ||
                    (order.side == OrderSide::SELL && book.bid_px[0] > 0.0 && order.price <= book.bid_px[0] + 1e-9);

                // POST_ONLY: the venue rejects a crossing order at
                // submit (HL Alo, OKX post_only, Binance LIMIT_MAKER
                // semantics). Never fills as TAKER, never enters
                // pending_. Caller checks order.rejected and emits
                // a venue-format error.
                if (order.type == OrderType::POST_ONLY && crosses) {
                    order.rejected = true;
                    bpt::common::log::debug(
                        "[MatchingEngine] POST_ONLY rejected (would cross): {} {} {} @ {} touch=({:.6f}/{:.6f})",
                        order.symbol,
                        (order.side == OrderSide::BUY ? "BUY" : "SELL"),
                        order.quantity,
                        order.price,
                        book.bid_px[0],
                        book.ask_px[0]);
                } else {
                    // Crossing-LIMIT path: if our price is at or through
                    // the touch, the crossing portion fills as TAKER at
                    // book level prices (NOT at our limit). This mirrors
                    // real exchange semantics — submitting a BUY at a
                    // price >= best ask immediately consumes the ask side
                    // and pays taker fees. Without this, AS's
                    // inventory-skew quotes (which intentionally cross to
                    // unwind position) get phantom-filled at the limit
                    // price, biasing backtest P&L by tens of bps per fill.
                    if (crosses && order.type == OrderType::LIMIT) {
                        fill_book_until(order, book, order.price, OrderType::LIMIT, fills);
                    }

                    // Residual (or non-crossing fully) rests in pending_
                    // with queue_ahead seeded from the book. POST_ONLY
                    // that didn't cross also rests here as a passive
                    // maker quote.
                    if (order.filled_qty < order.quantity - 1e-12) {
                        order.queue_ahead = book_qty_at_price(book, order.side, order.price);
                        order.queue_seeded = true;
                        pending_[key(order.exchange, order.symbol)].push_back(order);
                        bpt::common::log::debug("[MatchingEngine] Queued LIMIT {} {} {} @ {} queue_ahead={:.4f} cross_filled={:.4f}",
                                        order.symbol,
                                        (order.side == OrderSide::BUY ? "BUY" : "SELL"),
                                        order.quantity - order.filled_qty,
                                        order.price,
                                        order.queue_ahead,
                                        order.filled_qty);
                    }
                }
            } else {
                // No book yet. POST_ONLY can't be evaluated without a
                // book, but in practice the AS strategy doesn't quote
                // until it has BBO, so this branch shouldn't fire for
                // POST_ONLY. If it does, queue defensively — once the
                // book arrives, the legacy fill_crossing_limits path
                // would (incorrectly for POST_ONLY) fill as MAKER, but
                // that's no worse than the pre-POST_ONLY state.
                pending_[key(order.exchange, order.symbol)].push_back(order);
            }
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
    // MARKET = no price cap. BUY accepts any ask; SELL accepts any bid.
    const double cap = (order.side == OrderSide::BUY)
                           ? std::numeric_limits<double>::infinity()
                           : 0.0;
    fill_book_until(order, book, cap, OrderType::MARKET, out);

    if (order.filled_qty < order.quantity - 1e-12) {
        bpt::common::log::warn("[MatchingEngine] Market order {} partially filled: {}/{} — book too thin",
                       order.order_id,
                       order.filled_qty,
                       order.quantity);
    }
}

void MatchingEngine::fill_book_until(OpenOrder& order,
                                     const data::OrderBookRecord& book,
                                     double price_limit,
                                     OrderType report_type,
                                     std::vector<FillReport>& out) {
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

        // Stop once the level is worse than our price cap. BUY tops out
        // when the ask rises above price_limit; SELL stops when the bid
        // drops below price_limit.
        const bool acceptable = (order.side == OrderSide::BUY)
                                    ? (level_px <= price_limit + 1e-9)
                                    : (level_px >= price_limit - 1e-9);
        if (!acceptable)
            break;

        double fill_qty = std::min(remaining, level_sz);
        order.filled_qty += fill_qty;
        remaining -= fill_qty;

        FillReport r;
        r.order_id = order.order_id;
        r.client_order_id = order.client_order_id;
        r.exchange = order.exchange;
        r.symbol = order.symbol;
        r.order_type = report_type;
        r.side = order.side;
        r.liquidity_role = LiquidityRole::TAKER;  // crossing the book = TAKER
        r.original_qty = order.quantity;
        // For LIMIT orders we expose the original limit price (callers
        // care about both the limit and the actual fill price); for
        // MARKET it stays 0.
        r.order_price = (report_type == OrderType::LIMIT) ? order.price : 0.0;
        r.last_fill_qty = fill_qty;
        r.last_fill_price = level_px;
        r.cumulative_fill_qty = order.filled_qty;
        r.is_fully_filled = (remaining <= 1e-12);
        r.simulation_ts = current_ts_;
        out.push_back(r);
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

                                    // Queue-seeded orders fill via the
                                    // trade-print path (fill_against_trade);
                                    // skip them here to avoid double-counting.
                                    if (order.queue_seeded)
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
                                    // Order rested in pending_ until the
                                    // book moved to it → passive fill, MAKER.
                                    r.liquidity_role = LiquidityRole::MAKER;
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

double MatchingEngine::book_qty_at_price(const data::OrderBookRecord& book,
                                         OrderSide side,
                                         double price) {
    constexpr double kPriceTol = 1e-9;
    if (side == OrderSide::BUY) {
        // BUY → joins the bid queue at our price. Look for matching bid level.
        for (int i = 0; i < data::kOrderBookDepth; ++i) {
            if (book.bid_px[i] <= 0.0)
                continue;
            if (std::abs(book.bid_px[i] - price) < kPriceTol)
                return book.bid_sz[i];
        }
    } else {
        // SELL → joins the ask queue at our price.
        for (int i = 0; i < data::kOrderBookDepth; ++i) {
            if (book.ask_px[i] <= 0.0)
                continue;
            if (std::abs(book.ask_px[i] - price) < kPriceTol)
                return book.ask_sz[i];
        }
    }
    return 0.0;
}

void MatchingEngine::fill_against_trade(const data::TradeRecord& trade,
                                        std::vector<FillReport>& out) {
    auto pit = pending_.find(key(trade.exchange, trade.symbol));
    if (pit == pending_.end())
        return;
    auto& orders = pit->second;

    // Counter-side semantics: a SELL trade (taker sold) consumed the bid
    // book → only resting BUYs at price ≥ trade price participate. A BUY
    // trade consumed the ask book → only resting SELLs at price ≤ trade
    // price participate.
    //
    // Within the eligible orders, prints fan out FIFO across our own
    // resting orders ordered by submitted_ts. We take a single trade and
    // walk through orders in order — each consumes from its own queue
    // first, then fills from the residual. The next order in line sees
    // whatever volume is left after the previous order's queue+fill
    // consumption.
    //
    // Note: this doesn't model trade-volume that hits orders ahead of
    // the FIRST order in our pending list (those orders aren't ours).
    // queue_ahead absorbs that — it's the volume between the touch and
    // our first order. Correct as long as we trust the L5 snapshot.
    // Walk orders in submission order (FIFO). Each iteration may consume
    // some of the remaining trade volume. We index-iterate so we don't
    // invalidate iterators when erasing fully-filled orders at the end.
    double remaining_print = trade.quantity;
    for (auto& order : orders) {
        if (remaining_print <= 0.0)
            break;
        if (order.type != OrderType::LIMIT || !order.queue_seeded)
            continue;

        const bool eligible =
            (order.side == OrderSide::BUY  && trade.side == data::TradeSide::SELL && order.price >= trade.price - 1e-9) ||
            (order.side == OrderSide::SELL && trade.side == data::TradeSide::BUY  && order.price <= trade.price + 1e-9);
        if (!eligible)
            continue;

        // Drain queue_ahead first.
        const double consumed = std::min(remaining_print, order.queue_ahead);
        order.queue_ahead -= consumed;
        remaining_print -= consumed;
        if (remaining_print <= 0.0)
            continue;

        // Residual fills us, capped at our remaining qty.
        const double our_remaining = order.quantity - order.filled_qty;
        const double fill_qty = std::min(remaining_print, our_remaining);
        if (fill_qty <= 0.0)
            continue;

        order.filled_qty += fill_qty;
        remaining_print  -= fill_qty;

        FillReport r;
        r.order_id = order.order_id;
        r.client_order_id = order.client_order_id;
        r.exchange = order.exchange;
        r.symbol = order.symbol;
        r.order_type = OrderType::LIMIT;
        r.side = order.side;
        // Resting LIMIT consumed by an incoming print = MAKER, by definition.
        r.liquidity_role = LiquidityRole::MAKER;
        r.original_qty = order.quantity;
        r.order_price = order.price;
        r.last_fill_qty = fill_qty;
        // Real exchanges fill resting limits at the limit price (better
        // than the trade print for the maker), not at the print price.
        r.last_fill_price = order.price;
        r.cumulative_fill_qty = order.filled_qty;
        r.is_fully_filled = (order.filled_qty >= order.quantity - 1e-12);
        r.simulation_ts = current_ts_;
        out.push_back(r);
    }

    // Erase fully-filled queue-seeded orders. Non-seeded orders are
    // managed by fill_crossing_limits.
    orders.erase(
        std::remove_if(orders.begin(), orders.end(),
                       [](const OpenOrder& o) {
                           return o.queue_seeded &&
                                  o.filled_qty >= o.quantity - 1e-12;
                       }),
        orders.end());
}

}  // namespace bpt::backtester::matching
