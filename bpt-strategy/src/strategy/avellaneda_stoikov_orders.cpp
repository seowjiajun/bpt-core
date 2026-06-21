// AS order I/O: requote decisioning, limit-order placement, inventory unwind.

#include "strategy/clock/sim_clock.h"
#include "strategy/strategy/avellaneda_stoikov_strategy.h"

#include <messages/OrderType.h>
#include <messages/TimeInForce.h>
#include <messages/exec_inst.h>

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

static constexpr double kPriceScale = 1e8;

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
    const double eff_qty = effective_order_qty(st);

    const SuppressionState supp =
        supp_policy_.evaluate(st, net_qty, new_bid, new_ask, effective_max_inventory(st), eff_qty);

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

    // Legacy variable names retained for the side-decision blocks below
    // — match existing log-message key naming (`max_inv` vs `suppress`)
    // so the operational log format is unchanged by the refactor.
    const bool at_max_long = supp.inventory_bid;
    const bool at_max_short = supp.inventory_ask;
    const bool final_suppress_bids = supp.bid_signal();
    const bool final_suppress_asks = supp.ask_signal();

    // ── Bid side ──────────────────────────────────────────────────────────
    if (st.h_bid.live()) {
        // Adverse selection guard: cancel if mid has risen significantly since
        // we placed this bid — informed flow is pushing against us.
        const bool adverse =
            st.bid_placed_mid > 0.0 && (mid - st.bid_placed_mid) / st.bid_placed_mid > requote_threshold_;
        // Model drift: modify-in-place if the AS model wants a different price.
        const bool stale =
            st.last_bid_price > 0.0 && std::abs(new_bid - st.last_bid_price) / st.last_bid_price > requote_threshold_;
        if (at_max_long || adverse || final_suppress_bids) {
            // Hard cancel — don't amend. OM marks CancelPending before the
            // gateway call so the sync backtest path can't lose the
            // terminal status (see OrderManager::send_cancel comment).
            if (order_mgr_) {
                bpt::common::log::debug(kLog(),
                                        "Cancel bid order_id={} {} @ {} reason={}",
                                        st.h_bid.order_id(),
                                        st.symbol,
                                        st.exchange,
                                        at_max_long           ? "max_inv"
                                        : final_suppress_bids ? "suppress"
                                                              : "adverse");
                order_mgr_->send_cancel(st.h_bid);
            }
        } else if (stale) {
            if (order_mgr_) {
                double price = new_bid;
                if (st.tick_size > 0.0)
                    price = std::floor(price / st.tick_size) * st.tick_size;
                const int64_t price_fixed = static_cast<int64_t>(std::round(price * kPriceScale));
                const uint64_t qty_fp = static_cast<uint64_t>(std::round(eff_qty * 1e8));
                bpt::common::log::debug(kLog(),
                                        "Modify bid order_id={} {} @ {} → {:.6f}",
                                        st.h_bid.order_id(),
                                        st.symbol,
                                        st.exchange,
                                        price);
                order_mgr_->modify_order(order::ModifyOrderRequest{st.h_bid.order_id(),
                                                                   st.exchange_id,
                                                                   st.instrument_id,
                                                                   price_fixed,
                                                                   qty_fp});
            }
            st.last_bid_price = new_bid;
            st.bid_placed_mid = mid;
        }
    }

    if (!st.h_bid.valid() && !at_max_long && !final_suppress_bids) {
        auto h = send_limit_order(st, bpt::messages::OrderSide::BUY, new_bid, eff_qty);
        if (h.valid()) {
            st.h_bid = h;
            st.last_bid_price = new_bid;
            st.bid_placed_mid = mid;
        }
    }

    // ── Ask side ──────────────────────────────────────────────────────────
    if (st.h_ask.live()) {
        const bool adverse =
            st.ask_placed_mid > 0.0 && (st.ask_placed_mid - mid) / st.ask_placed_mid > requote_threshold_;
        const bool stale =
            st.last_ask_price > 0.0 && std::abs(new_ask - st.last_ask_price) / st.last_ask_price > requote_threshold_;
        if (at_max_short || adverse || final_suppress_asks) {
            if (order_mgr_) {
                bpt::common::log::debug(kLog(),
                                        "Cancel ask order_id={} {} @ {} reason={}",
                                        st.h_ask.order_id(),
                                        st.symbol,
                                        st.exchange,
                                        at_max_short          ? "max_inv"
                                        : final_suppress_asks ? "suppress"
                                                              : "adverse");
                order_mgr_->send_cancel(st.h_ask);
            }
        } else if (stale) {
            if (order_mgr_) {
                double price = new_ask;
                if (st.tick_size > 0.0)
                    price = std::ceil(price / st.tick_size) * st.tick_size;
                const int64_t price_fixed = static_cast<int64_t>(std::round(price * kPriceScale));
                const uint64_t qty_fp = static_cast<uint64_t>(std::round(eff_qty * 1e8));
                bpt::common::log::debug(kLog(),
                                        "Modify ask order_id={} {} @ {} → {:.6f}",
                                        st.h_ask.order_id(),
                                        st.symbol,
                                        st.exchange,
                                        price);
                order_mgr_->modify_order(order::ModifyOrderRequest{st.h_ask.order_id(),
                                                                   st.exchange_id,
                                                                   st.instrument_id,
                                                                   price_fixed,
                                                                   qty_fp});
            }
            st.last_ask_price = new_ask;
            st.ask_placed_mid = mid;
        }
    }

    if (!st.h_ask.valid() && !at_max_short && !final_suppress_asks) {
        auto h = send_limit_order(st, bpt::messages::OrderSide::SELL, new_ask, eff_qty);
        if (h.valid()) {
            st.h_ask = h;
            st.last_ask_price = new_ask;
            st.ask_placed_mid = mid;
        }
    }

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
    const auto vex_it = venue_exec_.find(st.exchange);
    if (vex_it == venue_exec_.end() || !vex_it->second.enabled) {
        bpt::common::log::debug(kLog(), "Venue {} not enabled — quote suppressed", st.exchange);
        return {};
    }

    if (!order_mgr_) {
        bpt::common::log::info(kLog(),
                               "{} {} {} @ {:.6f} (no gateway)",
                               (side == OrderSide::BUY ? "BID" : "ASK"),
                               st.symbol,
                               st.exchange,
                               price);
        return {};
    }

    // Note: OrderManager rounds BUY up and SELL down. For market-making, we want
    // the opposite (bid floors, ask ceils) to preserve spread width, so pre-round here.
    if (st.tick_size > 0.0) {
        if (side == OrderSide::BUY)
            price = std::floor(price / st.tick_size) * st.tick_size;
        else
            price = std::ceil(price / st.tick_size) * st.tick_size;
    }

    auto handle = order_mgr_->send_new_order(
        order::NewOrderRequest{
            .instrument_id = st.instrument_id,
            .exchange_id = st.exchange_id,
            .side = side,
            .type = OrderType::LIMIT,
            .tif = TimeInForce::GTC,
            .price = price,
            .qty = qty,
            .exec_inst = {.post_only = true},
        },
        kTagQuote);
    if (!handle.valid())
        return {};

    const uint64_t order_id = handle.order_id();
    bpt::common::log::info(kLog(),
                           "{} {} {} @ {:.6f} → order_id={}",
                           (side == OrderSide::BUY ? "BID" : "ASK"),
                           st.symbol,
                           st.exchange,
                           price,
                           order_id);

    st.queue.track(order_id, side, price, qty, bpt::common::util::WallClock::now_ns(), st.book);
    if (const auto* e = st.queue.lookup(order_id)) {
        bpt::common::log::info(kLog(),
                               "Queue track order_id={} side={} px={:.4f} qty={:.6f} "
                               "queue_ahead={:.6f} fill_prob={:.3f}",
                               order_id,
                               (side == OrderSide::BUY ? "BID" : "ASK"),
                               e->price,
                               e->our_qty,
                               e->queue_ahead,
                               st.queue.fill_probability(order_id));
    }
    return handle;
}

order::OrderHandle AvellanedaStoikovStrategy::send_unwind_order(InstrumentState& st,
                                                                bpt::messages::OrderSide::Value side,
                                                                double mid,
                                                                double qty,
                                                                uint8_t tag) {
    const auto vex_it = venue_exec_.find(st.exchange);
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
