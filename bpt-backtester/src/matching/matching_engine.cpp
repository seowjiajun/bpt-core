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

int64_t MatchingEngine::price_key(double price) {
    // Scaled to nanos-of-quote-unit. 1e9 covers crypto tick sizes (HL APE
    // = 1e-5, OKX BTC swap = 1e-1, Binance options = 1e-1) with margin.
    return static_cast<int64_t>(std::llround(price * 1.0e9));
}

void MatchingEngine::set_fill_callback(FillCallback cb) {
    std::lock_guard lock(mutex_);
    fill_cb_ = std::move(cb);
}

void MatchingEngine::set_latency_model(latency::LatencyModel* model) {
    std::lock_guard lock(mutex_);
    latency_ = model;
}

// ── Market event ──────────────────────────────────────────────────────────────

void MatchingEngine::on_market_event(const data::MarketEvent& event) {
    std::vector<FillReport> deliveries;

    {
        std::lock_guard lock(mutex_);
        const uint64_t event_ts = (event.type == data::MarketEvent::Type::ORDER_BOOK)
                                      ? std::get<data::OrderBookRecord>(event.payload).timestamp_ns
                                      : std::get<data::TradeRecord>(event.payload).timestamp_ns;

        // Drain any pending submits whose scheduled match time has arrived.
        // This must run before the book update so the order matches against
        // the book as it stood between this event and the previous one — the
        // exchange's latency-affected view, not the post-event view.
        drain_pending_submits(event_ts);

        std::vector<FillReport> fills;
        if (event.type == data::MarketEvent::Type::ORDER_BOOK) {
            const auto& ob = std::get<data::OrderBookRecord>(event.payload);
            current_ts_ = ob.timestamp_ns;
            const std::string book_key = key(ob.exchange, ob.symbol);
            // Phase 5: regen queue_ahead on resting orders before the book
            // is overwritten. This lets us see the *previous* level sizes
            // alongside the new ones to attribute the delta to cancels.
            apply_queue_regen(book_key, ob);
            books_[book_key] = ob;
            fill_crossing_limits(book_key, fills);
        } else {
            const auto& t = std::get<data::TradeRecord>(event.payload);
            current_ts_ = t.timestamp_ns;
            fill_against_trade(t, fills);
        }
        for (auto& f : fills)
            schedule_fill(std::move(f));

        drain_pending_fills(current_ts_, deliveries);
    }

    for (auto& fill : deliveries)
        if (fill_cb_)
            fill_cb_(fill);
}

// ── Order submission ──────────────────────────────────────────────────────────

OpenOrder MatchingEngine::submit_order(OpenOrder order) {
    std::vector<FillReport> deliveries;

    {
    std::lock_guard lock(mutex_);
    order.submitted_ts = current_ts_;

    // Synchronous POST_ONLY-cross rejection: real exchanges return this
    // in the ack frame (HL Alo, OKX post_only, Binance LIMIT_MAKER), so
    // the order server's HTTP response can carry the error string. The
    // check uses the current book — slightly optimistic vs. checking at
    // scheduled_match_ts, but POST_ONLY-cross is rare in normal AS
    // quoting and the synchronous-ack contract is more important.
    if (order.type == OrderType::POST_ONLY) {
        auto it = books_.find(key(order.exchange, order.symbol));
        if (it != books_.end()) {
            const auto& book = it->second;
            const bool crosses =
                (order.side == OrderSide::BUY  && book.ask_px[0] > 0.0 && order.price >= book.ask_px[0] - 1e-9) ||
                (order.side == OrderSide::SELL && book.bid_px[0] > 0.0 && order.price <= book.bid_px[0] + 1e-9);
            if (crosses) {
                order.rejected = true;
                bpt::common::log::debug(
                    "[MatchingEngine] POST_ONLY rejected (would cross): {} {} {} @ {} touch=({:.6f}/{:.6f})",
                    order.symbol,
                    (order.side == OrderSide::BUY ? "BUY" : "SELL"),
                    order.quantity, order.price, book.bid_px[0], book.ask_px[0]);
                return order;
            }
        }
    }

    // Defer the actual match by submit_to_match latency. If no model is
    // installed, scheduled_match_ts == current_ts_ and the order will drain
    // on the very next on_market_event, preserving pre-Phase-3 timing.
    uint64_t latency_ns = 0;
    if (latency_)
        latency_ns = latency_->draw(order.exchange, latency::LatencyLeg::SUBMIT_TO_MATCH);

    PendingSubmit ps;
    ps.order = order;
    ps.scheduled_match_ts = current_ts_ + latency_ns;
    pending_submits_.push_back(std::move(ps));

    // Drain any submits / fills whose scheduled times are already ≤ current_ts_.
    // With a null latency model (or zero latency for this venue) this fires the
    // just-queued order's match synchronously, preserving pre-Phase-3 semantics
    // where submit_order's fill_cb fired before return. With non-zero latency
    // this is a no-op — the order waits for a market event to advance time.
    drain_pending_submits(current_ts_);
    drain_pending_fills(current_ts_, deliveries);
    }  // unlock

    for (auto& fill : deliveries)
        if (fill_cb_)
            fill_cb_(fill);

    return order;
}

bool MatchingEngine::cancel_order(const std::string& exchange, const std::string& symbol, const std::string& order_id) {
    std::lock_guard lock(mutex_);

    // Resting orders.
    if (auto it = pending_.find(key(exchange, symbol)); it != pending_.end()) {
        auto& v = it->second;
        auto pos = std::find_if(v.begin(), v.end(),
                                [&](const OpenOrder& o) { return o.order_id == order_id; });
        if (pos != v.end()) {
            v.erase(pos);
            return true;
        }
    }

    // Orders still in the submit-to-match latency window.
    auto it = std::find_if(pending_submits_.begin(), pending_submits_.end(),
                           [&](const PendingSubmit& ps) {
                               return ps.order.exchange == exchange &&
                                      ps.order.symbol == symbol &&
                                      ps.order.order_id == order_id;
                           });
    if (it != pending_submits_.end()) {
        pending_submits_.erase(it);
        return true;
    }

    return false;
}

// ── Pending-submit / pending-fill machinery ──────────────────────────────────

void MatchingEngine::drain_pending_submits(uint64_t upto_ts) {
    if (pending_submits_.empty())
        return;

    // Sort by scheduled_match_ts; stable to preserve submission order
    // among orders with identical scheduled times.
    std::stable_sort(pending_submits_.begin(), pending_submits_.end(),
                     [](const PendingSubmit& a, const PendingSubmit& b) {
                         return a.scheduled_match_ts < b.scheduled_match_ts;
                     });

    std::vector<FillReport> fills;
    auto it = pending_submits_.begin();
    for (; it != pending_submits_.end(); ++it) {
        if (it->scheduled_match_ts > upto_ts)
            break;
        // Advance current_ts_ to the order's effective arrival time so
        // queue_ahead seeding and emitted fill timestamps reflect when
        // the match actually happened.
        if (it->scheduled_match_ts > current_ts_)
            current_ts_ = it->scheduled_match_ts;
        process_pending_submit(*it, fills);
    }
    pending_submits_.erase(pending_submits_.begin(), it);

    for (auto& f : fills)
        schedule_fill(std::move(f));
}

void MatchingEngine::process_pending_submit(PendingSubmit& ps, std::vector<FillReport>& out) {
    OpenOrder& order = ps.order;

    if (order.type == OrderType::MARKET) {
        auto it = books_.find(key(order.exchange, order.symbol));
        if (it != books_.end()) {
            fill_market(order, it->second, out);
        } else {
            bpt::common::log::warn("[MatchingEngine] No book for {}/{} — market order unfilled",
                           order.exchange, order.symbol);
        }
        return;
    }

    auto it = books_.find(key(order.exchange, order.symbol));
    if (it == books_.end()) {
        // Defensive: queue without seeding. fill_crossing_limits will pick
        // it up once a book arrives.
        pending_[key(order.exchange, order.symbol)].push_back(order);
        return;
    }
    const auto& book = it->second;

    const bool crosses =
        (order.side == OrderSide::BUY  && book.ask_px[0] > 0.0 && order.price >= book.ask_px[0] - 1e-9) ||
        (order.side == OrderSide::SELL && book.bid_px[0] > 0.0 && order.price <= book.bid_px[0] + 1e-9);

    // Crossing-LIMIT: TAKER fill against the book at scheduled_match_ts.
    // Note: a POST_ONLY that wasn't synchronously rejected at submit time
    // (because the book at submit time wasn't crossing) but now crosses
    // at match time would technically be rejected here. We simplify and
    // just queue it — the case is rare in AS quoting and the alternative
    // requires a deferred-rejection exec-report path.
    if (crosses && order.type == OrderType::LIMIT) {
        fill_book_until(order, book, order.price, OrderType::LIMIT, out);
    }

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

void MatchingEngine::schedule_fill(FillReport fill) {
    uint64_t latency_ns = 0;
    if (latency_)
        latency_ns = latency_->draw(fill.exchange, latency::LatencyLeg::MATCH_TO_REPORT);
    PendingFill pf;
    pf.scheduled_report_ts = fill.simulation_ts + latency_ns;
    pf.fill = std::move(fill);
    pending_fills_.push_back(std::move(pf));
}

void MatchingEngine::apply_queue_regen(const std::string& book_key,
                                       const data::OrderBookRecord& new_book) {
    // First book event for this instrument — nothing to compare against.
    auto old_it = books_.find(book_key);
    if (old_it == books_.end()) {
        traded_since_book_[book_key].clear();
        return;
    }
    const data::OrderBookRecord& old_book = old_it->second;

    auto pending_it = pending_.find(book_key);
    if (pending_it == pending_.end()) {
        traded_since_book_[book_key].clear();
        return;
    }

    const auto trades_at_it = traded_since_book_.find(book_key);
    const std::unordered_map<int64_t, double>* trades_at =
        (trades_at_it != traded_since_book_.end()) ? &trades_at_it->second : nullptr;

    for (auto& order : pending_it->second) {
        // Only queue-seeded orders carry meaningful queue_ahead. Backstop
        // orders (deeper than L5, or pre-book) fill via fill_crossing_limits.
        if (!order.queue_seeded)
            continue;

        const double old_size = book_qty_at_price(old_book, order.side, order.price);
        const double new_size = book_qty_at_price(new_book, order.side, order.price);
        // Level not visible in either snapshot — can't reason about cancels.
        // (Conservative: leaving queue_ahead alone here under-credits regen
        // when our level briefly drops off L5; a later book update with the
        // level back in view will resume normal accounting.)
        if (old_size <= 0.0 || new_size <= 0.0)
            continue;

        const double decrease = old_size - new_size;
        if (decrease <= 0.0)
            continue;  // level grew or unchanged; no cancels to attribute

        double traded_at_price = 0.0;
        if (trades_at) {
            const auto t_it = trades_at->find(price_key(order.price));
            if (t_it != trades_at->end())
                traded_at_price = t_it->second;
        }
        const double cancels_at_price = std::max(0.0, decrease - traded_at_price);
        if (cancels_at_price <= 0.0)
            continue;

        // Uniform-cancel attribution: under the assumption each unit on the
        // queue is equally likely to cancel, the expected number of cancels
        // ahead of us is queue_ahead × (cancels / level_size). Real micro-
        // structure is more nuanced — late-arriving size cancels less often,
        // and large icebergs reload — but uniform is the standard first cut
        // and the unbiased prior in the absence of per-order data.
        const double cancel_share = order.queue_ahead * (cancels_at_price / old_size);
        order.queue_ahead = std::max(0.0, order.queue_ahead - cancel_share);
    }

    traded_since_book_[book_key].clear();
}

void MatchingEngine::drain_pending_fills(uint64_t upto_ts, std::vector<FillReport>& out) {
    if (pending_fills_.empty())
        return;
    std::stable_sort(pending_fills_.begin(), pending_fills_.end(),
                     [](const PendingFill& a, const PendingFill& b) {
                         return a.scheduled_report_ts < b.scheduled_report_ts;
                     });
    auto it = pending_fills_.begin();
    for (; it != pending_fills_.end(); ++it) {
        if (it->scheduled_report_ts > upto_ts)
            break;
        out.push_back(std::move(it->fill));
    }
    pending_fills_.erase(pending_fills_.begin(), it);
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
                                    // Exception: queue_ahead == 0 means the
                                    // book lookup at submit time didn't find
                                    // a level matching our price (depth-1 BBO
                                    // book, or order deeper than L5). Those
                                    // orders have no useful queue info and
                                    // should fall through to the book-cross
                                    // backstop — otherwise they're never
                                    // fillable, which is wrong for orders
                                    // that the book legitimately walks
                                    // through.
                                    if (order.queue_seeded && order.queue_ahead > 0.0)
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
    // Phase 5: accumulate trade volume by level. apply_queue_regen reads
    // this on each book update to subtract trade-attributable size before
    // attributing the rest of the level decrease to cancels. Tracked
    // unconditionally — even when we currently hold no orders, a later
    // order at this level benefits from the partial accounting.
    traded_since_book_[key(trade.exchange, trade.symbol)][price_key(trade.price)] += trade.quantity;

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
