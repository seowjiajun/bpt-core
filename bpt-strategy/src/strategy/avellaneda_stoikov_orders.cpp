// AS order I/O: requote decisioning, limit-order placement, inventory unwind.

#include "strategy/clock/sim_clock.h"
#include "strategy/strategy/avellaneda_stoikov_strategy.h"

#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <cmath>

using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("AS");
    return l;
}
}  // namespace

void AvellanedaStoikovStrategy::maybe_requote(InstrumentState& st, const BboContext& ctx, const QuoteTarget& quotes) {
    const double net_qty = ctx.net_qty;
    const double mid = ctx.mid;
    const double new_bid = quotes.bid;
    const double new_ask = quotes.ask;
    // Honour exchange-error backoff before touching orders on this instrument.
    if (st.reject_backoff_until_ns > 0) {
        const uint64_t now_ns = bpt::strategy::clock::SimClock::now_ns();
        if (now_ns < st.reject_backoff_until_ns)
            return;
        // Backoff expired — clear it and allow quoting to resume.
        st.reject_backoff_until_ns = 0;
        bpt::common::log::info(kLog(), "Exchange backoff expired for {} @ {}, resuming quotes", st.symbol, st.exchange);
    }

    // Resolve per-tick sizing — adaptive when order_qty_fraction_ > 0,
    // fixed otherwise. Computed once so every order submit / modify /
    // unwind this tick uses the same qty (important: if equity /
    // price change between calls, downstream aggregation gets messy).
    // Also feeds the suppression policy's sizing inputs below.
    const double eff_qty = sizer_.effective_qty(st.last_mid, st.lot_size, last_equity_e8_, st.exchange);

    const SuppressionState supp = supp_policy_.evaluate(
        st, net_qty, new_bid, new_ask, sizer_.effective_max_inventory(st.last_mid, last_equity_e8_), eff_qty);

    // Info-level logging of the runtime triggers. Console reporting
    // consumes the same supp struct via get_strategy_state_json, so
    // these log lines and the rendered badge can't drift.
    if (supp.trend_bid || supp.trend_ask) {
        bpt::common::log::info(kLog(),
                               "{} trend suppress |Δ|={:.1f}bps > {:.1f}bps over {:.0f}s window — suppressing {}",
                               st.symbol,
                               std::abs(st.slow_drift_bps),
                               supp_policy_.config().slow_drift_suppress_bps,
                               slow_drift_window_s_,
                               supp.trend_ask ? "asks" : "bids");
    }
    if (supp.drift_bid || supp.drift_ask) {
        bpt::common::log::info(kLog(),
                               "{} drift suppress |µ|={:.1f}bps > {:.1f}bps — suppressing {}",
                               st.symbol,
                               std::abs(st.ewma_drift.value()) * 1e4,
                               supp_policy_.config().drift_suppress_bps,
                               supp.drift_ask ? "asks" : "bids");
    }
    if (supp.tox_bid) {
        bpt::common::log::info(kLog(),
                               "{} tox suppress bids: score={:.2f} < {:.2f}",
                               st.symbol,
                               st.tox_bid_toxicity,
                               supp_policy_.config().tox_suppress_threshold);
    }
    if (supp.tox_ask) {
        bpt::common::log::info(kLog(),
                               "{} tox suppress asks: score={:.2f} < {:.2f}",
                               st.symbol,
                               st.tox_ask_toxicity,
                               supp_policy_.config().tox_suppress_threshold);
    }
    if (supp.queue_bid) {
        bpt::common::log::info(kLog(),
                               "{} queue suppress bids: fp={:.5f} < {:.5f} at px={:.4f}",
                               st.symbol,
                               supp.fp_bid,
                               supp_policy_.config().queue_suppress_fill_prob_min,
                               new_bid);
    }
    if (supp.queue_ask) {
        bpt::common::log::info(kLog(),
                               "{} queue suppress asks: fp={:.5f} < {:.5f} at px={:.4f}",
                               st.symbol,
                               supp.fp_ask,
                               supp_policy_.config().queue_suppress_fill_prob_min,
                               new_ask);
    }

    const bool at_max_long = supp.inventory_bid;
    const bool at_max_short = supp.inventory_ask;
    const bool final_suppress_bids = supp.bid_signal();
    const bool final_suppress_asks = supp.ask_signal();

    // Manage one quote side: cancel/modify/place in priority order.
    auto manage_side = [&](order::OrderHandle& h, double& last_px, double& placed_mid_ref,
                            bool at_cap, bool suppressed, double new_px, bool is_bid) {
        if (h.live()) {
            const double adverse_move = is_bid ? (mid - placed_mid_ref) / placed_mid_ref
                                               : (placed_mid_ref - mid) / placed_mid_ref;
            const bool adverse = placed_mid_ref > 0.0 && adverse_move > requote_threshold_;
            const bool stale = last_px > 0.0 && std::abs(new_px - last_px) / last_px > requote_threshold_;
            if (at_cap || adverse || suppressed) {
                if (order_mgr_) {
                    bpt::common::log::debug(kLog(), "Cancel {} order_id={} {} @ {} reason={}",
                                            is_bid ? "bid" : "ask", h.order_id(), st.symbol, st.exchange,
                                            at_cap ? "max_inv" : suppressed ? "suppress" : "adverse");
                    order_mgr_->send_cancel(h);
                }
            } else if (stale) {
                if (order_mgr_) {
                    bpt::common::log::debug(kLog(), "Modify {} order_id={} {} @ {} → {:.6f}",
                                            is_bid ? "bid" : "ask", h.order_id(), st.symbol, st.exchange, new_px);
                    order_mgr_->modify_quote(h, new_px, eff_qty);
                }
                last_px = new_px;
                placed_mid_ref = mid;
            }
        }
        if (!h.valid() && !at_cap && !suppressed) {
            const auto side = is_bid ? bpt::messages::OrderSide::BUY : bpt::messages::OrderSide::SELL;
            if (auto nh = send_limit_order(st, side, new_px, eff_qty); nh.valid()) {
                h = nh;
                last_px = new_px;
                placed_mid_ref = mid;
            }
        }
    };

    manage_side(st.h_bid, st.last_bid_price, st.bid_placed_mid, at_max_long, final_suppress_bids, new_bid, true);
    manage_side(st.h_ask, st.last_ask_price, st.ask_placed_mid, at_max_short, final_suppress_asks, new_ask, false);

    // ── Active inventory unwind ────────────────────────────────────────────
    if (!st.h_unwind.valid()) {
        if (at_max_long) {
            auto h = send_unwind_order(st, bpt::messages::OrderSide::SELL, mid, eff_qty, kTagUnwindNormal);
            if (h.valid())
                st.h_unwind = h;
        } else if (at_max_short) {
            auto h = send_unwind_order(st, bpt::messages::OrderSide::BUY, mid, eff_qty, kTagUnwindNormal);
            if (h.valid())
                st.h_unwind = h;
        }
    }
}

order::OrderHandle AvellanedaStoikovStrategy::send_limit_order(InstrumentState& st,
                                                               bpt::messages::OrderSide::Value side,
                                                               double price,
                                                               double qty) {
    const auto vex_it = venue_exec_.find(st.exchange_id);
    if (vex_it == venue_exec_.end() || !vex_it->second.enabled || !order_mgr_)
        return {};

    auto handle = order_mgr_->send_quote(st.instrument_id, st.exchange_id, side, price, qty, kTagQuote);
    if (!handle.valid())
        return {};

    bpt::common::log::info(kLog(), "{} {} {} @ {:.6f} → order_id={}",
                           (side == OrderSide::BUY ? "BID" : "ASK"), st.symbol, st.exchange, price,
                           handle.order_id());
    st.queue.track(handle.order_id(), side, price, qty, bpt::common::util::WallClock::now_ns(), st.book);
    return handle;
}

order::OrderHandle AvellanedaStoikovStrategy::send_unwind_order(InstrumentState& st,
                                                                bpt::messages::OrderSide::Value side,
                                                                double mid,
                                                                double qty,
                                                                uint8_t tag) {
    const auto vex_it = venue_exec_.find(st.exchange_id);
    if (vex_it == venue_exec_.end() || !vex_it->second.enabled)
        return {};

    if (!order_mgr_) {
        bpt::common::log::info(kLog(),
                               "UNWIND {} {} @ {} mid={:.6f} (no gateway)",
                               (side == OrderSide::BUY ? "BUY" : "SELL"),
                               st.symbol,
                               st.exchange,
                               mid);
        return {};
    }

    // Use LIMIT IOC rather than MARKET to avoid OKX SPOT market-buy qty quirks
    // (OKX interprets SPOT market BUY sz as quote currency, not base).
    constexpr double kCrossBps = 20.0;
    const double cross_factor = 1.0 + (kCrossBps / 10000.0);
    const double price = (side == OrderSide::BUY) ? mid * cross_factor : mid / cross_factor;

    auto handle = order_mgr_->send_new_order(
        order::NewOrderRequest{
            .instrument_id = st.instrument_id,
            .exchange_id = st.exchange_id,
            .side = side,
            .type = OrderType::LIMIT,
            .tif = TimeInForce::IOC,
            .price = price,
            .qty = qty,
        },
        tag);
    if (!handle.valid())
        return {};

    bpt::common::log::info(kLog(),
                           "UNWIND {} {} @ {} price={:.6f} mid={:.6f} → order_id={}",
                           (side == OrderSide::BUY ? "BUY" : "SELL"),
                           st.symbol,
                           st.exchange,
                           price,
                           mid,
                           handle.order_id());
    return handle;
}

}  // namespace bpt::strategy::strategy
