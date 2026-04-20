#include "strategy/order/paper_fill_engine.h"

#include <algorithm>
#include <cmath>

namespace bpt::strategy::order {

namespace {

constexpr double kPriceScale = 1e8;

// Convert a price_e8 fixed-point to natural units for BBO comparison.
double e8_to_natural(int64_t price_e8) {
    return static_cast<double>(price_e8) / kPriceScale;
}

int64_t natural_to_e8(double price) {
    return static_cast<int64_t>(std::llround(price * kPriceScale));
}

// Returns true if an order would cross the given BBO at placement.
// BUY crosses if its price >= best ask; SELL crosses if its price <= best bid.
bool crosses_bbo(bpt::messages::OrderSide::Value side,
                 int64_t price_e8,
                 const PaperFillEngine::Order& /*o*/,
                 double bid,
                 double ask) {
    const double p = e8_to_natural(price_e8);
    if (side == bpt::messages::OrderSide::BUY)
        return ask > 0.0 && p >= ask;
    return bid > 0.0 && p <= bid;
}

}  // namespace

void PaperFillEngine::emit_reject(const Order& o,
                                  bpt::messages::RejectReason::Value reason,
                                  uint64_t ts_ns) {
    PaperFillEvent ev;
    ev.order_id = o.order_id;
    ev.exchange_id = o.exchange_id;
    ev.instrument_id = o.instrument_id;
    ev.status = bpt::messages::ExecStatus::REJECTED;
    ev.side = o.side;
    ev.order_type = o.order_type;
    ev.price_e8 = o.price_e8;
    ev.filled_qty_e8 = 0;
    ev.remaining_qty_e8 = o.quantity_e8;
    ev.reject_reason = reason;
    ev.ts_ns = ts_ns;
    pending_.push_back(ev);
}

void PaperFillEngine::emit_ack(const Order& o, uint64_t ts_ns) {
    PaperFillEvent ev;
    ev.order_id = o.order_id;
    ev.exchange_id = o.exchange_id;
    ev.instrument_id = o.instrument_id;
    ev.status = bpt::messages::ExecStatus::ACKED;
    ev.side = o.side;
    ev.order_type = o.order_type;
    ev.price_e8 = o.price_e8;
    ev.filled_qty_e8 = 0;
    ev.remaining_qty_e8 = o.quantity_e8;
    ev.ts_ns = ts_ns;
    pending_.push_back(ev);
}

void PaperFillEngine::emit_fill(const Order& o, int64_t fill_price_e8, uint64_t ts_ns) {
    PaperFillEvent ev;
    ev.order_id = o.order_id;
    ev.exchange_id = o.exchange_id;
    ev.instrument_id = o.instrument_id;
    ev.status = bpt::messages::ExecStatus::FILLED;
    ev.side = o.side;
    ev.order_type = o.order_type;
    ev.price_e8 = fill_price_e8;
    ev.filled_qty_e8 = o.quantity_e8;
    ev.remaining_qty_e8 = 0;
    ev.ts_ns = ts_ns;
    pending_.push_back(ev);
}

void PaperFillEngine::emit_cancel(const Order& o, uint64_t ts_ns) {
    PaperFillEvent ev;
    ev.order_id = o.order_id;
    ev.exchange_id = o.exchange_id;
    ev.instrument_id = o.instrument_id;
    ev.status = bpt::messages::ExecStatus::CANCELLED;
    ev.side = o.side;
    ev.order_type = o.order_type;
    ev.price_e8 = o.price_e8;
    ev.filled_qty_e8 = 0;
    ev.remaining_qty_e8 = o.quantity_e8;
    ev.ts_ns = ts_ns;
    pending_.push_back(ev);
}

void PaperFillEngine::submit(const Order& o, uint64_t submit_ns) {
    using OT = bpt::messages::OrderType;
    using TIF = bpt::messages::TimeInForce;

    if (o.order_type == OT::MARKET) {
        // Out of scope for MVP — reject cleanly rather than guess at
        // slippage semantics. Strategies that need market orders can
        // cross via aggressive LIMIT IOC.
        emit_reject(o, bpt::messages::RejectReason::RISK_REJECTED, submit_ns);
        return;
    }

    const auto it = bbo_.find(o.instrument_id);
    const bool have_bbo = (it != bbo_.end() && it->second.valid);

    // IOC / FOK: try to cross immediately, else reject.
    if (o.tif == TIF::IOC || o.tif == TIF::FOK) {
        if (!have_bbo) {
            // Closest available enum — EXCHANGE_ERROR covers "we
            // don't have the market data needed to match this order."
            emit_reject(o, bpt::messages::RejectReason::EXCHANGE_ERROR, submit_ns);
            return;
        }
        const auto& bbo = it->second;
        if (!crosses_bbo(o.side, o.price_e8, o, bbo.bid, bbo.ask)) {
            emit_reject(o, bpt::messages::RejectReason::INVALID_PRICE, submit_ns);
            return;
        }
        const double fill_price =
            (o.side == bpt::messages::OrderSide::BUY) ? bbo.ask : bbo.bid;
        emit_fill(o, natural_to_e8(fill_price), submit_ns);
        return;
    }

    // POST_ONLY: rejects if it would cross the book.
    if (o.order_type == OT::POST_ONLY && have_bbo) {
        const auto& bbo = it->second;
        if (crosses_bbo(o.side, o.price_e8, o, bbo.bid, bbo.ask)) {
            emit_reject(o, bpt::messages::RejectReason::INVALID_PRICE, submit_ns);
            return;
        }
    }

    // GTC and POST_ONLY (non-crossing) rest in the book until a trade
    // print sweeps them.
    emit_ack(o, submit_ns);
    resting_[o.instrument_id].push_back(o);
}

void PaperFillEngine::cancel(uint64_t order_id,
                             bpt::messages::ExchangeId::Value /*exchange_id*/,
                             uint64_t instrument_id,
                             uint64_t cancel_ns) {
    auto it = resting_.find(instrument_id);
    if (it == resting_.end())
        return;
    auto& vec = it->second;
    auto oit = std::find_if(vec.begin(), vec.end(),
                            [order_id](const Order& o) { return o.order_id == order_id; });
    if (oit == vec.end())
        return;
    const Order o = *oit;
    vec.erase(oit);
    emit_cancel(o, cancel_ns);
}

void PaperFillEngine::cancel_all(bpt::messages::ExchangeId::Value /*exchange_id*/,
                                 uint64_t instrument_id,
                                 uint64_t cancel_ns) {
    auto it = resting_.find(instrument_id);
    if (it == resting_.end())
        return;
    for (const auto& o : it->second)
        emit_cancel(o, cancel_ns);
    it->second.clear();
}

void PaperFillEngine::on_bbo(uint64_t instrument_id,
                             double bid,
                             double ask,
                             uint64_t /*ts_ns*/) {
    auto& b = bbo_[instrument_id];
    b.bid = bid;
    b.ask = ask;
    b.valid = (bid > 0.0 && ask > 0.0 && ask >= bid);
}

void PaperFillEngine::on_trade(uint64_t instrument_id,
                               double price,
                               double /*qty*/,
                               uint64_t ts_ns) {
    if (price <= 0.0)
        return;
    auto it = resting_.find(instrument_id);
    if (it == resting_.end())
        return;

    const double trade_px = price;
    auto& vec = it->second;

    // Check every resting order; swept ones are filled and removed.
    auto new_end = std::remove_if(vec.begin(), vec.end(),
        [this, trade_px, ts_ns](const Order& o) {
            const double px = e8_to_natural(o.price_e8);
            const bool sweep =
                (o.side == bpt::messages::OrderSide::BUY) ? (trade_px <= px)
                                                          : (trade_px >= px);
            if (sweep) {
                // Fill at our resting (maker) price, not the trade
                // print — mirrors how passive makers actually execute
                // against an aggressor.
                emit_fill(o, o.price_e8, ts_ns);
                return true;  // remove
            }
            return false;
        });
    vec.erase(new_end, vec.end());
}

int PaperFillEngine::drain(int limit,
                           const std::function<void(const PaperFillEvent&)>& cb) {
    int n = 0;
    while (n < limit && !pending_.empty()) {
        cb(pending_.front());
        pending_.pop_front();
        ++n;
    }
    return n;
}

std::size_t PaperFillEngine::resting_count() const {
    std::size_t total = 0;
    for (const auto& [k, vec] : resting_)
        total += vec.size();
    return total;
}

}  // namespace bpt::strategy::order
