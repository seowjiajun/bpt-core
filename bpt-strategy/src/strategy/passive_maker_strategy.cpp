#include "strategy/strategy/passive_maker_strategy.h"

#include "strategy/clock/sim_clock.h"
#include "strategy/md/subscribe_helpers.h"
#include "strategy/refdata/exchange_id.h"
#include "strategy/venue/min_order_value.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/InstrumentType.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <cmath>

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("PassiveMaker");
    return l;
}
}  // namespace

// ── Constructor ──────────────────────────────────────────────────────────────

PassiveMakerStrategy::PassiveMakerStrategy(uint64_t correlation_id,
                                           const config::StrategyConfig& cfg,
                                           refdata::IRefdataClient& refdata,
                                           md::IMdClient* md,
                                           order::OrderManager* order_mgr)
    : correlation_id_(correlation_id),
      half_spread_bps_(cfg.params["half_spread_bps"].value<double>().value_or(15.0)),
      inventory_penalty_(cfg.params["inventory_penalty"].value<double>().value_or(0.0)),
      requote_threshold_bps_(cfg.params["requote_threshold_bps"].value<double>().value_or(50.0)),
      qty_per_quote_(cfg.params["qty_per_quote"].value<double>().value_or(50.0)),
      max_inventory_(cfg.params["max_inventory"].value<double>().value_or(500.0)),
      spread_vol_mult_(cfg.params["spread_vol_mult"].value<double>().value_or(0.0)),
      requote_vol_mult_(cfg.params["requote_vol_mult"].value<double>().value_or(0.0)),
      vol_window_size_(static_cast<std::size_t>(cfg.params["vol_window_size"].value<int64_t>().value_or(60))),
      vol_sample_interval_ns_(
          static_cast<uint64_t>(cfg.params["vol_sample_interval_ms"].value<double>().value_or(1000.0) * 1e6)),
      regime_gating_enabled_(cfg.params["regime_gating_enabled"].value<bool>().value_or(false)),
      regime_cfg_{
          static_cast<std::size_t>(cfg.params["regime_window_size"].value<int64_t>().value_or(60)),
          static_cast<uint64_t>(cfg.params["regime_sample_interval_ms"].value<double>().value_or(1000.0) * 1e6),
          cfg.params["regime_quiet_vol_bps_per_min"].value<double>().value_or(5.0),
          cfg.params["regime_trend_threshold_z"].value<double>().value_or(1.0),
          static_cast<uint64_t>(cfg.params["regime_chop_cooldown_s"].value<double>().value_or(120.0) * 1e9),
      },
      vol_gate_cfg_{
          cfg.params["vol_gate_max_bps"].value<double>().value_or(0.0),
          static_cast<uint64_t>(cfg.params["vol_gate_window_ms"].value<double>().value_or(1000.0) * 1e6),
          static_cast<uint64_t>(cfg.params["vol_gate_halt_ms"].value<double>().value_or(5000.0) * 1e6),
      },
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr) {
    bpt::common::log::info(kLog(),
                           "H={:.1f}bps c={:.6g} requote={:.1f}bps qty={:.4g} max_inv={:.4g}",
                           half_spread_bps_,
                           inventory_penalty_,
                           requote_threshold_bps_,
                           qty_per_quote_,
                           max_inventory_);
    bpt::common::log::info(kLog(),
                           "linear_vol_scaling: spread_mult={:.2f} requote_mult={:.2f} "
                           "vol_window={} sample_interval={}ms",
                           spread_vol_mult_,
                           requote_vol_mult_,
                           vol_window_size_,
                           vol_sample_interval_ns_ / 1'000'000);
    if (regime_gating_enabled_) {
        bpt::common::log::info(kLog(),
                               "regime_gating: ENABLED window={} sample_interval={}ms "
                               "quiet_vol={:.1f}bps/min trend_z={:.2f} chop_cooldown={}s",
                               regime_cfg_.window_size,
                               regime_cfg_.sample_interval_ns / 1'000'000,
                               regime_cfg_.quiet_vol_bps_per_min,
                               regime_cfg_.trend_threshold_z,
                               regime_cfg_.chop_cooldown_ns / 1'000'000'000ULL);
    } else {
        bpt::common::log::info(kLog(), "regime_gating: disabled");
    }
    if (vol_gate_cfg_.max_bps_per_window > 0.0) {
        bpt::common::log::info(kLog(),
                               "vol_gate max_bps={:.1f} window={}ms halt={}ms",
                               vol_gate_cfg_.max_bps_per_window,
                               vol_gate_cfg_.window_ns / 1'000'000,
                               vol_gate_cfg_.halt_duration_ns / 1'000'000);
    } else {
        bpt::common::log::info(kLog(), "vol_gate disabled");
    }
}

// ── IStrategy lifecycle ──────────────────────────────────────────────────────

void PassiveMakerStrategy::start() {
    refdata_.subscribe(correlation_id_, CanonicalResolver::build_filters(instruments_, md_exchanges_));
}

void PassiveMakerStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    if (!state_.empty()) {
        bpt::common::log::debug(kLog(), "Ignoring duplicate snapshot ({} instruments)", cache.size());
        return;
    }
    for (const auto& r : CanonicalResolver::resolve_instruments(cache, instruments_, md_exchanges_)) {
        InstrumentState st(vol_gate_cfg_, vol_window_size_, vol_sample_interval_ns_, regime_cfg_);
        st.instrument_id = r.instrument_id;
        st.symbol = r.instrument.symbol;
        st.exchange = r.instrument.exchange;
        st.exchange_id = r.exchange_id;
        st.tick_size = r.instrument.tick_size;
        st.lot_size = r.instrument.lot_size;

        bpt::common::log::info(kLog(),
                               "Instrument [{}] {} @ {} tick={} lot={}",
                               r.instrument_id,
                               r.instrument.symbol,
                               r.instrument.exchange,
                               r.instrument.tick_size,
                               r.instrument.lot_size);
        state_.emplace(r.instrument_id, std::move(st));
    }
    bpt::common::log::info(kLog(), "Resolved {} instrument(s)", state_.size());

    if (md_client_)
        // Depth=1 is enough for L1-driven microprice; we don't need deeper.
        md_client_->subscribe(correlation_id_, md::build_subscriptions(state_, /*depth=*/1));
}

void PassiveMakerStrategy::on_delta(const refdata::Instrument& /*inst*/,
                                    bpt::messages::DeltaUpdateType::Value /*type*/) {}

void PassiveMakerStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;
    InstrumentState& st = it->second;

    st.bid = tick.bidPrice();
    st.ask = tick.askPrice();
    st.bid_size = tick.bidQty();
    st.ask_size = tick.askQty();
    st.last_book_ns = bpt::strategy::clock::SimClock::now_ns();
    if (st.last_book_ns == 0)
        st.last_book_ns = tick.timestampNs();

    if (st.bid <= 0.0 || st.ask <= 0.0)
        return;

    const double mid = (st.bid + st.ask) * 0.5;

    // Feed the realized-vol estimator (legacy path) and the regime
    // classifier (primary path). Both are cheap; updating both lets us
    // toggle between them via config without losing history.
    st.vol_est.update(mid, st.last_book_ns);
    st.regime.update(mid, st.last_book_ns);

    // Update vol gate based on mid move.
    if (st.vol_gate.update_and_check(mid, st.last_book_ns)) {
        bpt::common::log::info(kLog(), "{} vol_gate halted — cancelling resting quotes", st.symbol);
        if (st.bid_order_id != 0 && order_mgr_) {
            order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, st.instrument_id);
            st.bid_order_id = 0;
        }
        if (st.ask_order_id != 0 && order_mgr_) {
            order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, st.instrument_id);
            st.ask_order_id = 0;
        }
        return;
    }
    if (st.vol_gate.is_halted(st.last_book_ns))
        return;
    if (st.flatten_in_progress)
        return;

    // Regime classifier — primary gate. CHOPPY → pause: cancel resting
    // orders if any, and don't quote. Strategy resumes when regime exits
    // CHOPPY (vol drops below quiet threshold or trend builds).
    if (regime_gating_enabled_) {
        const auto regime = st.regime.classify(st.last_book_ns);
        if (regime == RegimeClassifier::Regime::CHOPPY) {
            if (st.bid_order_id != 0 && order_mgr_) {
                order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, st.instrument_id);
                st.bid_order_id = 0;
            }
            if (st.ask_order_id != 0 && order_mgr_) {
                order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, st.instrument_id);
                st.ask_order_id = 0;
            }
            return;
        }
    }

    const double fv = compute_fair_value(st.bid, st.ask, st.bid_size, st.ask_size);
    if (fv <= 0.0)
        return;

    // Three triggers — check in priority order.
    const bool no_quotes_resting = (st.bid_order_id == 0 && st.ask_order_id == 0);
    const bool drift_trip = drift_exceeds_threshold(st, fv);
    const bool inventory_breach =
        std::abs(st.inventory) >= max_inventory_ &&
        ((st.inventory > 0 && st.bid_order_id != 0) || (st.inventory < 0 && st.ask_order_id != 0));

    if (no_quotes_resting || drift_trip || inventory_breach) {
        requote(st, fv, st.last_book_ns);
    }
}

void PassiveMakerStrategy::on_trade(const bpt::messages::MdTrade& /*tick*/) {}

void PassiveMakerStrategy::on_order_book(const bpt::messages::MdOrderBook& /*book*/) {
    // L1-driven; we ignore deeper book updates. on_bbo carries everything we need.
}

bool PassiveMakerStrategy::drift_exceeds_threshold(const InstrumentState& st, double fv_now) const {
    if (st.fv_at_place <= 0.0)
        return true;  // never placed → treat as drift trip to seed quotes
    const double drift_bps = std::abs(fv_now - st.fv_at_place) / st.fv_at_place * 1e4;
    // Vol-scaled threshold: high realized vol → wider threshold so we don't
    // requote on every move and hand the market repeated chances to pick us off.
    const double rv_per_min_bps = st.vol_est.ready() ? annualized_to_per_minute_bps(st.vol_est.realized_vol()) : 0.0;
    const double effective_threshold = scale_with_vol(requote_threshold_bps_, requote_vol_mult_, rv_per_min_bps);
    return drift_bps > effective_threshold;
}

// ── Quoting ──────────────────────────────────────────────────────────────────

void PassiveMakerStrategy::requote(InstrumentState& st, double fv, uint64_t /*now_ns*/) {
    // Cancel both sides — simplest correct behaviour. Production would
    // skip cancelling whichever side hasn't actually moved, but the
    // savings are small at this strategy's quote cadence (minutes per
    // quote, not milliseconds).
    if (order_mgr_) {
        if (st.bid_order_id != 0) {
            order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, st.instrument_id);
            st.bid_order_id = 0;
        }
        if (st.ask_order_id != 0) {
            order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, st.instrument_id);
            st.ask_order_id = 0;
        }
    }

    // Reservation price: shift FV away from inventory. Long → r drops →
    // ask drops (more attractive to sellers, so we get hit and unwind).
    const double r = fv - st.inventory * inventory_penalty_;

    // Vol-scaled half-spread: high realized vol → wider spread to compensate
    // for the higher expected adverse markout per fill.
    const double rv_per_min_bps = st.vol_est.ready() ? annualized_to_per_minute_bps(st.vol_est.realized_vol()) : 0.0;
    const double effective_half_bps = scale_with_vol(half_spread_bps_, spread_vol_mult_, rv_per_min_bps);
    const double half_spread = r * (effective_half_bps / 1e4);
    double bid_px = round_to_tick(r - half_spread, st.tick_size, /*round_down=*/true);
    double ask_px = round_to_tick(r + half_spread, st.tick_size, /*round_down=*/false);

    // Sanity: if the rounded prices end up crossing the touch they would
    // be POST_ONLY-rejected. Pull back to one tick inside the touch.
    if (bid_px >= st.ask)
        bid_px = round_to_tick(st.ask - st.tick_size, st.tick_size, /*round_down=*/true);
    if (ask_px <= st.bid)
        ask_px = round_to_tick(st.bid + st.tick_size, st.tick_size, /*round_down=*/false);

    // Inventory-driven side suppression: when at the long cap, only
    // post the ask (the unwind side); when at the short cap, only the bid.
    const bool post_bid = st.inventory < max_inventory_;
    const bool post_ask = st.inventory > -max_inventory_;

    if (post_bid && bid_px > 0.0)
        st.bid_order_id = place_side(st, OrderSide::BUY, bid_px);
    if (post_ask && ask_px > 0.0)
        st.ask_order_id = place_side(st, OrderSide::SELL, ask_px);

    if (st.bid_order_id != 0)
        st.bid_order_price = bid_px;
    if (st.ask_order_id != 0)
        st.ask_order_price = ask_px;
    st.fv_at_place = fv;

    bpt::common::log::info(kLog(),
                           "{} requote inv={:+.4f} fv={:.6f} bid={:.6f} ask={:.6f} "
                           "half={:.1f}bps (base={:.1f}+vol_mult*rv_min={:.1f}bps)",
                           st.symbol,
                           st.inventory,
                           fv,
                           bid_px,
                           ask_px,
                           effective_half_bps,
                           half_spread_bps_,
                           rv_per_min_bps);
}

uint64_t PassiveMakerStrategy::place_side(InstrumentState& st, OrderSide::Value side, double price) {
    if (!order_mgr_)
        return 0;
    // Bump qty to clear the venue's min-notional floor before placing.
    // HL rejects orders whose `price × qty < $10` (venue-wide rule, not
    // exposed in refdata). bump_qty_for_min_notional returns
    // qty_per_quote_ unchanged when (a) the venue has no floor or (b)
    // qty_per_quote_ × price already clears it — so this is a no-op
    // for venues that don't impose the rule, and only widens otherwise.
    const double qty =
        bpt::strategy::venue::bump_qty_for_min_notional(qty_per_quote_,
                                                        price,
                                                        st.lot_size,
                                                        bpt::strategy::venue::min_notional_usd(st.exchange));

    // LIMIT + GTC: the HL order-gateway adapter translates this to
    // tif="Alo" (Add-Liquidity-Only) on the wire, which the matching
    // engine recognises as POST_ONLY and synchronously rejects if it
    // would cross. The round_to_tick step above already pulls bid/ask
    // one tick inside the touch, so this should never trigger in
    // practice — belt-and-braces for the rare race-condition case.
    const uint64_t oid =
        order_mgr_->place_order(st.instrument_id, st.exchange_id, side, OrderType::LIMIT, TimeInForce::GTC, price, qty);
    if (oid == 0) {
        bpt::common::log::warn(kLog(),
                               "{} place_order rejected ({} @ {:.6f})",
                               st.symbol,
                               (side == OrderSide::BUY ? "BID" : "ASK"),
                               price);
        return 0;
    }
    order_to_instrument_[oid] = st.instrument_id;
    return oid;
}

// ── Execution reports ───────────────────────────────────────────────────────

void PassiveMakerStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const uint64_t order_id = rpt.orderId();
    auto inst_it = order_to_instrument_.find(order_id);
    if (inst_it == order_to_instrument_.end())
        return;

    auto st_it = state_.find(inst_it->second);
    if (st_it == state_.end())
        return;
    InstrumentState& st = st_it->second;

    const auto status = rpt.status();

    // ExecutionReport carries a *cumulative* filledQty (scaled 1e8). The
    // per-event fill delta = filledQty − last seen for this order. Both
    // PARTIAL and FILLED can carry fill volume; account for both.
    if (status == ExecStatus::PARTIAL || status == ExecStatus::FILLED) {
        const uint64_t cum_filled_e8 = rpt.filledQty();
        const uint64_t last_e8 = st.last_filled_qty_e8[order_id];
        if (cum_filled_e8 > last_e8) {
            const double delta_qty = static_cast<double>(cum_filled_e8 - last_e8) / 1e8;
            const double delta_px = static_cast<double>(rpt.price()) / 1e8;
            const double signed_delta = (rpt.side() == bpt::messages::OrderSide::BUY) ? delta_qty : -delta_qty;
            st.inventory += signed_delta;
            st.last_filled_qty_e8[order_id] = cum_filled_e8;
            bpt::common::log::info(kLog(),
                                   "{} fill side={} delta_qty={:.4g} px={:.6f} new_inv={:+.4g}",
                                   st.symbol,
                                   (rpt.side() == bpt::messages::OrderSide::BUY ? "BUY" : "SELL"),
                                   delta_qty,
                                   delta_px,
                                   st.inventory);
        }
    }

    const bool is_terminal =
        (status == ExecStatus::FILLED) || (status == ExecStatus::CANCELLED) || (status == ExecStatus::REJECTED);

    if (is_terminal) {
        if (st.bid_order_id == order_id)
            st.bid_order_id = 0;
        if (st.ask_order_id == order_id)
            st.ask_order_id = 0;
        st.last_filled_qty_e8.erase(order_id);
        order_to_instrument_.erase(order_id);
    }
}

// ── Shutdown flatten ────────────────────────────────────────────────────────

void PassiveMakerStrategy::on_shutdown_flatten() {
    for (auto& [id, st] : state_) {
        st.flatten_in_progress = true;
        if (order_mgr_) {
            // Cancel resting POST_ONLY quotes first.
            if (st.bid_order_id != 0) {
                order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, st.instrument_id);
                st.bid_order_id = 0;
            }
            if (st.ask_order_id != 0) {
                order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, st.instrument_id);
                st.ask_order_id = 0;
            }
            // IOC unwind any non-zero inventory.
            if (std::abs(st.inventory) > 1e-9) {
                const auto side = st.inventory > 0 ? OrderSide::SELL : OrderSide::BUY;
                const double mid = (st.bid + st.ask) * 0.5;
                if (mid > 0.0) {
                    // Cross by 5bps to ensure the IOC takes immediately.
                    const double price =
                        (side == OrderSide::SELL) ? st.bid * (1.0 - 5.0 / 1e4) : st.ask * (1.0 + 5.0 / 1e4);
                    const uint64_t oid = order_mgr_->place_order(st.instrument_id,
                                                                 st.exchange_id,
                                                                 side,
                                                                 OrderType::LIMIT,
                                                                 TimeInForce::IOC,
                                                                 price,
                                                                 std::abs(st.inventory));
                    if (oid != 0)
                        order_to_instrument_[oid] = st.instrument_id;
                    bpt::common::log::warn(kLog(),
                                           "{} shutdown unwind {} {:.4g} @ {:.6f} (inv was {:+.4g})",
                                           st.symbol,
                                           (side == OrderSide::BUY ? "BUY" : "SELL"),
                                           std::abs(st.inventory),
                                           price,
                                           st.inventory);
                }
            }
        }
    }
}

bool PassiveMakerStrategy::has_pending_flatten() const {
    for (const auto& [_, st] : state_) {
        if (st.bid_order_id != 0 || st.ask_order_id != 0)
            return true;
        if (std::abs(st.inventory) > 1e-9)
            return true;
    }
    return false;
}

}  // namespace bpt::strategy::strategy
