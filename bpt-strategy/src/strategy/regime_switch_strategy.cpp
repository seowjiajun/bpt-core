#include "strategy/strategy/regime_switch_strategy.h"

#include "strategy/md/subscribe_helpers.h"
#include "strategy/refdata/exchange_id.h"
#include "strategy/strategy/hurst_estimator.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/InstrumentType.h>
#include <messages/OrderType.h>
#include <messages/RejectSource.h>
#include <messages/TimeInForce.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <chrono>
#include <cmath>
#include <numeric>

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectSource;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

namespace {
// Sub-module logger — auto-prefixed with "RS" via %(logger) in the default
// log pattern. Lazy-initialised because bpt::common::logging::init() runs
// after static initialisation.
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("RS");
    return l;
}
}  // namespace

static constexpr double kPriceScale = 1e8;
static constexpr double kQtyScale = 1e8;                           // must match OrderGateway's kScale
static constexpr uint64_t kBarIntervalDefault = 1'000'000'000ULL;  // 1s in ns

const char* RegimeSwitchStrategy::regime_name(Regime r) {
    switch (r) {
        case Regime::WARMING_UP:
            return "WARMING_UP";
        case Regime::GRID:
            return "GRID";
        case Regime::MOMENTUM:
            return "MOMENTUM";
        case Regime::NEUTRAL:
            return "NEUTRAL";
        case Regime::TRANSITIONING:
            return "TRANSITIONING";
    }
    return "UNKNOWN";
}

// ── Constructor ─────────────────────────────────────────────────────────────

RegimeSwitchStrategy::RegimeSwitchStrategy(uint64_t correlation_id,
                                           const config::StrategyConfig& cfg,
                                           refdata::IRefdataClient& refdata,
                                           md::IMdClient* md,
                                           order::OrderManager* order_mgr)
    : hurst_window_(static_cast<size_t>(cfg.params["hurst_window"].value<int64_t>().value_or(100))),
      hurst_eval_ticks_(static_cast<int>(cfg.params["hurst_eval_ticks"].value<int64_t>().value_or(10))),
      mean_revert_threshold_(cfg.params["mean_revert_threshold"].value<double>().value_or(0.45)),
      trend_threshold_(cfg.params["trend_threshold"].value<double>().value_or(0.55)),
      hysteresis_(cfg.params["hysteresis"].value<double>().value_or(0.05)),
      min_regime_dwell_(static_cast<int>(cfg.params["min_regime_dwell"].value<int64_t>().value_or(5))),
      grid_levels_count_(static_cast<int>(cfg.params["grid_levels"].value<int64_t>().value_or(3))),
      grid_spacing_bps_(cfg.params["grid_spacing_bps"].value<double>().value_or(20.0)),
      grid_qty_usd_(cfg.params["grid_qty_usd"].value<double>().value_or(100.0)),
      grid_max_position_usd_(cfg.params["grid_max_position_usd"].value<double>().value_or(500.0)),
      grid_recenter_bps_(cfg.params["grid_recenter_bps"].value<double>().value_or(50.0)),
      ema_fast_period_(static_cast<int>(cfg.params["ema_fast_period"].value<int64_t>().value_or(12))),
      ema_slow_period_(static_cast<int>(cfg.params["ema_slow_period"].value<int64_t>().value_or(26))),
      atr_period_(static_cast<int>(cfg.params["atr_period"].value<int64_t>().value_or(14))),
      atr_stop_mult_(cfg.params["atr_stop_multiplier"].value<double>().value_or(2.0)),
      atr_target_mult_(cfg.params["atr_target_multiplier"].value<double>().value_or(3.0)),
      momentum_qty_usd_(cfg.params["momentum_qty_usd"].value<double>().value_or(200.0)),
      aggress_bps_(cfg.params["aggress_bps"].value<double>().value_or(10.0)),
      max_spread_bps_(cfg.params["max_spread_bps"].value<double>().value_or(30.0)),
      bar_interval_ns_(cfg.params["bar_interval_s"].value<double>()
                           ? static_cast<uint64_t>(*cfg.params["bar_interval_s"].value<double>() * 1e9)
                           : kBarIntervalDefault),
      order_book_depth_(static_cast<uint8_t>(cfg.params["order_book_depth"].value<int64_t>().value_or(0))),
      correlation_id_(correlation_id),
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr) {
    if (hurst_window_ > kMaxHurstWindow)
        hurst_window_ = kMaxHurstWindow;
    if (grid_levels_count_ > kMaxGridLevels)
        grid_levels_count_ = kMaxGridLevels;

    // Order ID generation is handled by OrderManager (globally unique across all strategies).

    bpt::common::log::info(kLog(),
                           "hurst_window={} eval_bars={} mr_thresh={:.2f} "
                           "trend_thresh={:.2f} hysteresis={:.2f}",
                           hurst_window_,
                           hurst_eval_ticks_,
                           mean_revert_threshold_,
                           trend_threshold_,
                           hysteresis_);
    bpt::common::log::info(kLog(),
                           "grid: levels={} spacing={:.0f}bps qty_usd={:.0f} "
                           "max_pos_usd={:.0f} recenter={:.0f}bps",
                           grid_levels_count_,
                           grid_spacing_bps_,
                           grid_qty_usd_,
                           grid_max_position_usd_,
                           grid_recenter_bps_);
    bpt::common::log::info(kLog(),
                           "momentum: ema_fast={} ema_slow={} atr={} "
                           "stop_mult={:.1f} target_mult={:.1f} qty_usd={:.0f}",
                           ema_fast_period_,
                           ema_slow_period_,
                           atr_period_,
                           atr_stop_mult_,
                           atr_target_mult_,
                           momentum_qty_usd_);
    bpt::common::log::info(kLog(),
                           "execution: aggress={:.1f}bps max_spread={:.1f}bps bar_interval={:.1f}s depth={}",
                           aggress_bps_,
                           max_spread_bps_,
                           bar_interval_ns_ / 1e9,
                           order_book_depth_);
    bpt::common::log::info(kLog(),
                           "risk: max_pos_usd={} max_order_usd={}",
                           cfg.risk.max_position_usd,
                           cfg.risk.max_order_size_usd);
    bpt::common::log::info(kLog(), "order IDs managed by OrderManager (globally unique)");
}

// ── IStrategy ───────────────────────────────────────────────────────────────

void RegimeSwitchStrategy::start() {
    for (const auto& ex : md_exchanges_)
        bpt::common::log::info(kLog(), "MD exchange: {}", ex);

    refdata_.subscribe(correlation_id_, CanonicalResolver::build_filters(instruments_, md_exchanges_));
}

void RegimeSwitchStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    // Guard against duplicate snapshots from Sindri re-broadcasts.
    if (!state_.empty()) {
        bpt::common::log::debug(kLog(), "Ignoring duplicate snapshot ({} instruments)", cache.size());
        return;
    }

    bpt::common::log::info(kLog(), "Snapshot ({} instruments), resolving...", cache.size());
    order_to_instrument_.clear();
    positions_.clear_all();

    for (const auto& r : CanonicalResolver::resolve_instruments(cache, instruments_, md_exchanges_)) {
        InstrumentState st;
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

    if (!md_client_)
        return;
    auto subs = md::build_subscriptions(state_, order_book_depth_);
    bpt::common::log::info(kLog(), "Subscribing MD to {} instrument(s)", subs.size());
    md_client_->subscribe(correlation_id_, subs);
}

void RegimeSwitchStrategy::on_delta(const refdata::Instrument& /*inst*/,
                                    bpt::messages::DeltaUpdateType::Value /*update_type*/) {}

void RegimeSwitchStrategy::on_trade(const bpt::messages::MdTrade& /*tick*/) {}

// ── Order Book ──────────────────────────────────────────────────────────────

void RegimeSwitchStrategy::on_order_book(const bpt::messages::MdOrderBook& book) {
    auto it = state_.find(book.instrumentId());
    if (it == state_.end())
        return;

    // For now, log depth info periodically for diagnostics.
    // Future: use order book imbalance to bias regime detection or signal generation.
    // The BBO is already published separately by MdGateway alongside the order book,
    // so on_bbo still drives all strategy logic.
}

// ── BBO ─────────────────────────────────────────────────────────────────────

void RegimeSwitchStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;

    InstrumentState& st = it->second;
    const double bid = tick.bidPrice();
    const double ask = tick.askPrice();
    if (bid <= 0.0 || ask <= 0.0 || ask <= bid)
        return;

    st.bid = bid;
    st.ask = ask;
    st.last_bbo_ns = tick.timestampNs();
    const double mid = (bid + ask) * 0.5;

    // ── Update time bar ──
    const uint64_t bar_interval = static_cast<uint64_t>(bar_interval_ns_);
    if (st.bar_start_ns == 0) {
        // First tick — start first bar.
        st.bar_start_ns = st.last_bbo_ns;
        st.bar_open = mid;
        st.bar_high = mid;
        st.bar_low = mid;
        st.bar_close = mid;
    } else {
        st.bar_close = mid;
        if (mid > st.bar_high)
            st.bar_high = mid;
        if (mid < st.bar_low)
            st.bar_low = mid;

        // Check if bar is complete.
        if (st.last_bbo_ns - st.bar_start_ns >= bar_interval) {
            on_bar_close(st);
            // Start new bar.
            st.bar_start_ns = st.last_bbo_ns;
            st.bar_open = mid;
            st.bar_high = mid;
            st.bar_low = mid;
            st.bar_close = mid;
        }
    }

    // ── Between bar evaluations, still check momentum exit if active ──
    if (st.regime == Regime::MOMENTUM && st.has_momentum_position)
        momentum_check_exit(st, mid);

    // ── Transition timeout — force-complete after 10s ──
    if (st.regime == Regime::TRANSITIONING) {
        constexpr uint64_t kTransitionTimeoutNs = 10'000'000'000ULL;  // 10s
        if (st.last_bbo_ns - st.transition_start_ns > kTransitionTimeoutNs) {
            bpt::common::log::warn(kLog(),
                                   "{} transition timeout — force-completing ({} pending cancels)",
                                   st.symbol,
                                   st.pending_cancels.size());
            for (uint64_t oid : st.pending_cancels)
                order_to_instrument_.erase(oid);
            st.pending_cancels.clear();
            if (st.close_order_id != 0) {
                order_to_instrument_.erase(st.close_order_id);
                st.close_order_id = 0;
            }
            st.transition = TransitionPhase::READY;
            check_transition_complete(st);
        }
    }
}

// ── Time Bar Close ──────────────────────────────────────────────────────────

void RegimeSwitchStrategy::on_bar_close(InstrumentState& st) {
    const double close = st.bar_close;

    // ── Compute log-return from bar closes ──
    if (st.prev_bar_close > 0.0) {
        const double lr = std::log(close / st.prev_bar_close);
        st.log_returns[st.return_head] = lr;
        st.return_head = (st.return_head + 1) % hurst_window_;
        if (st.return_count < hurst_window_)
            ++st.return_count;
    }
    st.prev_bar_close = close;

    // ── Update bar ATR (proper true range including gap from prev close) ──
    double tr = st.bar_high - st.bar_low;
    if (st.prev_bar_close > 0.0) {
        tr = std::max({tr, std::abs(st.bar_high - st.prev_bar_close), std::abs(st.bar_low - st.prev_bar_close)});
    }
    if (st.bar_atr_warmup == 0) {
        st.bar_atr = tr;
        st.bar_atr_warmup = 1;
    } else {
        const double alpha = 2.0 / (atr_period_ + 1.0);
        st.bar_atr = alpha * tr + (1.0 - alpha) * st.bar_atr;
        ++st.bar_atr_warmup;
    }

    // ── Update momentum EMA indicators on bar close ──
    momentum_update_indicators(st, close);

    // ── Periodic Hurst evaluation (every N bars) ──
    ++st.bars_since_hurst_eval;
    if (st.bars_since_hurst_eval < hurst_eval_ticks_)
        return;
    st.bars_since_hurst_eval = 0;

    // Need enough data for Hurst.
    if (st.return_count < 20) {
        if (st.regime == Regime::WARMING_UP)
            bpt::common::log::debug(kLog(), "{} warming up: {}/{} returns", st.symbol, st.return_count, hurst_window_);
        return;
    }

    const double hurst = compute_hurst_multi_window(st.log_returns.data(), st.return_count, hurst_window_);
    st.last_hurst = hurst;

    const Regime new_regime = classify_regime(hurst, st.regime);
    ++st.regime_dwell;

    // ── Regime change ──
    if (new_regime != st.regime && st.regime != Regime::TRANSITIONING && st.regime_dwell >= min_regime_dwell_) {
        bpt::common::log::info(kLog(),
                               "{} regime change: {} → {} (H={:.3f})",
                               st.symbol,
                               regime_name(st.regime),
                               regime_name(new_regime),
                               hurst);
        begin_transition(st, new_regime);
        return;
    }

    const double mid = (st.bid + st.ask) * 0.5;

    // ── Spread filter — skip actions when spread is too wide ──
    const double spread_bps = (st.ask - st.bid) / mid * 10000.0;
    const bool spread_ok = spread_bps <= max_spread_bps_;

    // ── Act within current regime ──
    if (st.regime == Regime::GRID) {
        if (!spread_ok) {
            bpt::common::log::debug(kLog(),
                                    "{} grid skipped — spread {:.1f}bps > max {:.1f}bps",
                                    st.symbol,
                                    spread_bps,
                                    max_spread_bps_);
        } else if (st.grid.active_count == 0) {
            grid_build(st);
        } else if (st.grid.grid_center > 0.0) {
            const double drift_bps = std::abs(mid - st.grid.grid_center) / st.grid.grid_center * 10000.0;
            if (drift_bps > grid_recenter_bps_) {
                bpt::common::log::info(kLog(),
                                       "{} grid recenter: mid={:.2f} center={:.2f} drift={:.1f}bps",
                                       st.symbol,
                                       mid,
                                       st.grid.grid_center,
                                       drift_bps);
                grid_cancel_all(st);
                grid_build(st);
            }
        }
    } else if (st.regime == Regime::MOMENTUM) {
        if (!spread_ok) {
            bpt::common::log::debug(kLog(),
                                    "{} momentum skipped — spread {:.1f}bps > max {:.1f}bps",
                                    st.symbol,
                                    spread_bps,
                                    max_spread_bps_);
        } else if (!st.has_momentum_position) {
            momentum_check_signal(st);
        } else {
            momentum_check_exit(st, mid);
        }
    }

    bpt::common::log::info(kLog(),
                           "{} H={:.3f} regime={} dwell={}/{} mid={:.2f} atr={:.2f} spread={:.1f}bps",
                           st.symbol,
                           hurst,
                           regime_name(st.regime),
                           st.regime_dwell,
                           min_regime_dwell_,
                           mid,
                           st.bar_atr,
                           spread_bps);
}

// ── Exec Reports ────────────────────────────────────────────────────────────

void RegimeSwitchStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const uint64_t order_id = rpt.orderId();
    auto inst_it = order_to_instrument_.find(order_id);
    if (inst_it == order_to_instrument_.end())
        return;

    auto st_it = state_.find(inst_it->second);
    if (st_it == state_.end())
        return;

    InstrumentState& st = st_it->second;
    const auto status = rpt.status();

    if (status == ExecStatus::ACKED)
        return;

    if (status == ExecStatus::REJECTED) {
        const auto src = rpt.rejectSource();
        const bool gateway_reject = (src == RejectSource::GATEWAY || src == RejectSource::RISK);
        if (gateway_reject)
            bpt::common::log::error(kLog(),
                                    "{} exec order_id={} REJECTED reason={} source={}",
                                    st.symbol,
                                    order_id,
                                    bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                    bpt::messages::RejectSource::c_str(src));
        else
            bpt::common::log::warn(kLog(),
                                   "{} exec order_id={} REJECTED reason={} source={}",
                                   st.symbol,
                                   order_id,
                                   bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                   bpt::messages::RejectSource::c_str(src));
    } else {
        bpt::common::log::info(kLog(),
                               "{} exec order_id={} status={} filled={:.6f} price={:.2f}",
                               st.symbol,
                               order_id,
                               bpt::messages::ExecStatus::c_str(status),
                               static_cast<double>(rpt.filledQty()) / kQtyScale,
                               static_cast<double>(rpt.price()) / kPriceScale);
    }

    // Track fill.
    if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
        positions_.on_fill(st.instrument_id, st.exchange_id, rpt.side(), rpt.filledQty(), rpt.price());

        if (const auto pos = positions_.get(st.instrument_id, st.exchange_id)) {
            bpt::common::log::info(kLog(),
                                   "{} pos net_qty={:.6f} rpnl={:.4f}",
                                   st.symbol,
                                   static_cast<double>(pos->net_qty) / kQtyScale,
                                   pos->realized_pnl);
        }
    }

    bool is_terminal =
        (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED);
    if (!is_terminal)
        return;

    // Track consecutive rejections for backoff.
    // Gateway rejects are bpt-strategy bugs — don't count them toward the exchange backoff.
    if (status == ExecStatus::REJECTED) {
        const auto src = rpt.rejectSource();
        const bool gateway_reject = (src == RejectSource::GATEWAY || src == RejectSource::RISK);
        if (!gateway_reject) {
            ++st.consecutive_rejects;
            if (st.consecutive_rejects >= 3) {
                constexpr uint64_t kCooldownNs = 30'000'000'000ULL;
                st.reject_cooldown_until_ns = st.last_bbo_ns + kCooldownNs;
                bpt::common::log::warn(kLog(),
                                       "{} pausing orders for 30s after {} consecutive rejects",
                                       st.symbol,
                                       st.consecutive_rejects);
            }
        }
    } else {
        st.consecutive_rejects = 0;
    }

    // ── Transition handling ──
    if (st.regime == Regime::TRANSITIONING) {
        if (st.transition == TransitionPhase::CANCELLING_ORDERS) {
            st.pending_cancels.erase(order_id);
            check_transition_complete(st);
        } else if (st.transition == TransitionPhase::CLOSING_POSITION) {
            if (order_id == st.close_order_id) {
                st.close_order_id = 0;
                st.has_momentum_position = false;
                st.transition = TransitionPhase::READY;
                check_transition_complete(st);
            }
        }
    }
    // ── Grid fill handling ──
    else if (st.regime == Regime::GRID) {
        grid_on_fill(st, order_id, rpt);
    }
    // ── Momentum fill handling ──
    else if (st.regime == Regime::MOMENTUM) {
        if (order_id == st.momentum_order_id) {
            if (status == ExecStatus::FILLED) {
                if (st.has_momentum_position) {
                    // This was a close order.
                    st.has_momentum_position = false;
                    st.momentum_order_id = 0;
                    bpt::common::log::info(kLog(), "{} momentum position closed", st.symbol);
                } else {
                    // This was an entry.
                    st.has_momentum_position = true;
                    st.momentum_entry_price = static_cast<double>(rpt.price()) / kPriceScale;

                    if (st.momentum_side == bpt::messages::OrderSide::BUY) {
                        st.momentum_stop = st.momentum_entry_price - st.bar_atr * atr_stop_mult_;
                        st.momentum_target = st.momentum_entry_price + st.bar_atr * atr_target_mult_;
                    } else {
                        st.momentum_stop = st.momentum_entry_price + st.bar_atr * atr_stop_mult_;
                        st.momentum_target = st.momentum_entry_price - st.bar_atr * atr_target_mult_;
                    }
                    bpt::common::log::info(kLog(),
                                           "{} momentum entered: side={} price={:.2f} stop={:.2f} target={:.2f}",
                                           st.symbol,
                                           st.momentum_side == bpt::messages::OrderSide::BUY ? "BUY" : "SELL",
                                           st.momentum_entry_price,
                                           st.momentum_stop,
                                           st.momentum_target);
                }
            } else {
                // Order cancelled or rejected.
                st.momentum_order_id = 0;
            }
        }
    }

    // Clean up order tracking.
    order_to_instrument_.erase(order_id);

    // Also clean from grid levels and track active count.
    for (auto& lvl : st.grid.levels) {
        if (lvl.order_id == order_id) {
            lvl.order_id = 0;
            if (st.grid.active_count > 0)
                --st.grid.active_count;
        }
    }
}

// ── Hurst Exponent (Rescaled Range) ─────────────────────────────────────────

RegimeSwitchStrategy::Regime RegimeSwitchStrategy::classify_regime(double hurst, Regime current) const {
    // Apply hysteresis to prevent rapid flipping.
    if (current == Regime::GRID) {
        if (hurst > trend_threshold_)
            return Regime::MOMENTUM;
        if (hurst > mean_revert_threshold_ + hysteresis_)
            return Regime::NEUTRAL;
        return Regime::GRID;
    }

    if (current == Regime::MOMENTUM) {
        if (hurst < mean_revert_threshold_)
            return Regime::GRID;
        if (hurst < trend_threshold_ - hysteresis_)
            return Regime::NEUTRAL;
        return Regime::MOMENTUM;
    }

    // From NEUTRAL or WARMING_UP.
    if (hurst < mean_revert_threshold_)
        return Regime::GRID;
    if (hurst > trend_threshold_)
        return Regime::MOMENTUM;
    return Regime::NEUTRAL;
}

// ── Fee Lookup ──────────────────────────────────────────────────────────────

double RegimeSwitchStrategy::get_round_trip_fee_bps(const InstrumentState& st) const {
    const auto fee = refdata_.fee_cache().get(st.exchange_id, st.instrument_id, st.last_bbo_ns);
    if (fee) {
        // Grid orders are maker; take-profits are also maker (GTC limit).
        // Round-trip = 2x maker fee.
        return 2.0 * static_cast<double>(fee->maker_bps);
    }
    // Conservative fallback: 20bps round-trip (10bps each side).
    return 20.0;
}

// ── Grid Sub-Strategy ───────────────────────────────────────────────────────

void RegimeSwitchStrategy::grid_build(InstrumentState& st) {
    const double mid = (st.bid + st.ask) * 0.5;
    if (mid <= 0.0)
        return;

    // Reject cooldown — don't spam orders if gateway is rejecting.
    if (st.reject_cooldown_until_ns > 0 && st.last_bbo_ns < st.reject_cooldown_until_ns) {
        bpt::common::log::debug(kLog(), "{} grid_build skipped — in reject cooldown", st.symbol);
        return;
    }
    st.reject_cooldown_until_ns = 0;
    st.consecutive_rejects = 0;

    st.grid.grid_center = mid;
    st.grid.active_count = 0;

    // Volatility-scaled spacing: use bar ATR if available, else fall back to fixed bps.
    double spacing;
    if (st.bar_atr > 0.0 && st.bar_atr_warmup >= atr_period_) {
        // Space grid levels at 1x ATR apart — adapts to current volatility.
        spacing = st.bar_atr;
        bpt::common::log::info(kLog(),
                               "{} grid spacing from ATR: {:.2f} ({:.1f}bps)",
                               st.symbol,
                               spacing,
                               spacing / mid * 10000.0);
    } else {
        spacing = mid * grid_spacing_bps_ / 10000.0;
    }

    // Ensure spacing covers round-trip fees — otherwise every completed
    // grid cycle loses money.
    const double fee_bps = get_round_trip_fee_bps(st);
    const double min_spacing = mid * fee_bps / 10000.0 * 1.5;  // 1.5x fees for margin
    if (spacing < min_spacing) {
        bpt::common::log::info(kLog(),
                               "{} grid spacing {:.2f} < fee floor {:.2f} (fees={:.1f}bps), widening",
                               st.symbol,
                               spacing,
                               min_spacing,
                               fee_bps);
        spacing = min_spacing;
    }

    const double target_base = grid_qty_usd_ / mid;
    double lot = st.lot_size > 0.0 ? st.lot_size : 1.0 / kQtyScale;
    const double rounded_base = std::floor(target_base / lot) * lot;
    if (rounded_base <= 0.0) {
        bpt::common::log::warn(kLog(), "{} grid qty too small for lot_size", st.symbol);
        return;
    }

    for (int i = 1; i <= grid_levels_count_; ++i) {
        // Buy levels below mid.
        double buy_price = mid - spacing * i;
        if (st.tick_size > 0.0)
            buy_price = std::floor(buy_price / st.tick_size) * st.tick_size;

        int idx = i - 1;
        st.grid.levels[idx].price = buy_price;
        st.grid.levels[idx].side = OrderSide::BUY;
        st.grid.levels[idx].is_take_profit = false;
        st.grid.levels[idx].order_id =
            send_order(st, OrderSide::BUY, OrderType::LIMIT, TimeInForce::GTC, buy_price, rounded_base);

        // Sell levels above mid.
        double sell_price = mid + spacing * i;
        if (st.tick_size > 0.0)
            sell_price = std::ceil(sell_price / st.tick_size) * st.tick_size;

        idx = grid_levels_count_ + i - 1;
        st.grid.levels[idx].price = sell_price;
        st.grid.levels[idx].side = OrderSide::SELL;
        st.grid.levels[idx].is_take_profit = false;
        st.grid.levels[idx].order_id =
            send_order(st, OrderSide::SELL, OrderType::LIMIT, TimeInForce::GTC, sell_price, rounded_base);
    }
    st.grid.active_count = grid_levels_count_ * 2;

    bpt::common::log::info(
        kLog(),
        "{} grid built: center={:.2f} spacing={:.2f} ({:.1f}bps) levels={} qty={:.6f} fees={:.1f}bps",
        st.symbol,
        mid,
        spacing,
        spacing / mid * 10000.0,
        grid_levels_count_ * 2,
        rounded_base,
        fee_bps);
}

void RegimeSwitchStrategy::grid_cancel_all(InstrumentState& st) {
    for (auto& lvl : st.grid.levels) {
        if (lvl.order_id != 0) {
            cancel_order(st, lvl.order_id);
            st.pending_cancels.insert(lvl.order_id);
        }
    }
    st.grid.active_count = 0;
}

void RegimeSwitchStrategy::grid_on_fill(InstrumentState& st,
                                        uint64_t order_id,
                                        const bpt::messages::ExecutionReport& rpt) {
    if (rpt.status() != ExecStatus::FILLED)
        return;

    const double fill_price = static_cast<double>(rpt.price()) / kPriceScale;
    const double mid = (st.bid + st.ask) * 0.5;

    // Check position limit before placing take-profit.
    const int64_t net = positions_.net_qty(st.instrument_id, st.exchange_id);
    const double pos_usd = std::abs(static_cast<double>(net) / kQtyScale * mid);
    if (pos_usd > grid_max_position_usd_) {
        bpt::common::log::warn(kLog(),
                               "{} grid position limit reached: ${:.0f} > ${:.0f}",
                               st.symbol,
                               pos_usd,
                               grid_max_position_usd_);
        return;
    }

    // Find which grid level was filled and place the take-profit opposite.
    for (auto& lvl : st.grid.levels) {
        if (lvl.order_id != order_id)
            continue;

        // TP spacing must clear round-trip fees.
        const double fee_bps = get_round_trip_fee_bps(st);
        double tp_spacing;
        if (st.bar_atr > 0.0 && st.bar_atr_warmup >= atr_period_) {
            tp_spacing = st.bar_atr;
        } else {
            tp_spacing = st.grid.grid_center * grid_spacing_bps_ / 10000.0;
        }
        const double min_tp_spacing = mid * fee_bps / 10000.0 * 1.5;
        if (tp_spacing < min_tp_spacing)
            tp_spacing = min_tp_spacing;

        double tp_price;
        OrderSide::Value tp_side;

        if (lvl.side == OrderSide::BUY) {
            tp_price = fill_price + tp_spacing;
            tp_side = OrderSide::SELL;
        } else {
            tp_price = fill_price - tp_spacing;
            tp_side = OrderSide::BUY;
        }

        if (st.tick_size > 0.0) {
            if (tp_side == OrderSide::SELL)
                tp_price = std::ceil(tp_price / st.tick_size) * st.tick_size;
            else
                tp_price = std::floor(tp_price / st.tick_size) * st.tick_size;
        }

        lvl.order_id = send_order(st,
                                  tp_side,
                                  OrderType::LIMIT,
                                  TimeInForce::GTC,
                                  tp_price,
                                  static_cast<double>(rpt.filledQty()) / kQtyScale);
        lvl.price = tp_price;
        lvl.side = tp_side;
        lvl.is_take_profit = true;

        bpt::common::log::info(kLog(),
                               "{} grid TP: {} @ {:.2f} → {} @ {:.2f} (spacing={:.2f} fees={:.1f}bps)",
                               st.symbol,
                               rpt.side() == OrderSide::BUY ? "BUY" : "SELL",
                               fill_price,
                               tp_side == OrderSide::BUY ? "BUY" : "SELL",
                               tp_price,
                               tp_spacing,
                               fee_bps);
        break;
    }
}

// ── Momentum Sub-Strategy ───────────────────────────────────────────────────

void RegimeSwitchStrategy::momentum_update_indicators(InstrumentState& st, double mid) {
    // EMA update (now called on bar close, not every tick).
    if (st.ema_warmup == 0) {
        st.ema_fast = mid;
        st.ema_slow = mid;
        st.ema_warmup = 1;
    } else {
        const double alpha_fast = 2.0 / (ema_fast_period_ + 1.0);
        const double alpha_slow = 2.0 / (ema_slow_period_ + 1.0);
        st.ema_fast = alpha_fast * mid + (1.0 - alpha_fast) * st.ema_fast;
        st.ema_slow = alpha_slow * mid + (1.0 - alpha_slow) * st.ema_slow;
        ++st.ema_warmup;
    }
}

void RegimeSwitchStrategy::momentum_check_signal(InstrumentState& st) {
    if (st.ema_warmup < ema_slow_period_ || st.bar_atr_warmup < atr_period_)
        return;
    if (st.bar_atr < 1e-10)
        return;

    // Reject cooldown — don't spam orders if gateway is rejecting.
    if (st.reject_cooldown_until_ns > 0 && st.last_bbo_ns < st.reject_cooldown_until_ns)
        return;

    const double mid = (st.bid + st.ask) * 0.5;

    if (st.ema_fast > st.ema_slow) {
        // Bullish crossover — go long.
        const double price = st.ask * (1.0 + aggress_bps_ / 10000.0);
        const double target_base = momentum_qty_usd_ / mid;
        double lot = st.lot_size > 0.0 ? st.lot_size : 1.0 / kQtyScale;
        double rounded = std::floor(target_base / lot) * lot;
        if (rounded <= 0.0)
            return;
        st.momentum_side = OrderSide::BUY;
        st.momentum_order_id = send_order(st, OrderSide::BUY, OrderType::LIMIT, TimeInForce::IOC, price, rounded);
        bpt::common::log::info(kLog(),
                               "{} momentum BUY signal: ema_fast={:.2f} > ema_slow={:.2f} atr={:.2f}",
                               st.symbol,
                               st.ema_fast,
                               st.ema_slow,
                               st.bar_atr);

    } else if (st.ema_fast < st.ema_slow) {
        // Bearish crossover — go short.
        const double price = st.bid * (1.0 - aggress_bps_ / 10000.0);
        const double target_base = momentum_qty_usd_ / mid;
        double lot = st.lot_size > 0.0 ? st.lot_size : 1.0 / kQtyScale;
        double rounded = std::floor(target_base / lot) * lot;
        if (rounded <= 0.0)
            return;
        st.momentum_side = OrderSide::SELL;
        st.momentum_order_id = send_order(st, OrderSide::SELL, OrderType::LIMIT, TimeInForce::IOC, price, rounded);
        bpt::common::log::info(kLog(),
                               "{} momentum SELL signal: ema_fast={:.2f} < ema_slow={:.2f} atr={:.2f}",
                               st.symbol,
                               st.ema_fast,
                               st.ema_slow,
                               st.bar_atr);
    }
}

void RegimeSwitchStrategy::momentum_check_exit(InstrumentState& st, double mid) {
    if (!st.has_momentum_position)
        return;
    // Don't fire duplicate closes while one is in-flight.
    if (st.momentum_order_id != 0)
        return;

    bool should_exit = false;
    std::string reason;

    if (st.momentum_side == OrderSide::BUY) {
        if (mid <= st.momentum_stop) {
            should_exit = true;
            reason = "stop_loss";
        } else if (mid >= st.momentum_target) {
            should_exit = true;
            reason = "take_profit";
        } else if (st.ema_fast < st.ema_slow) {
            should_exit = true;
            reason = "ema_cross";
        }
    } else {
        if (mid >= st.momentum_stop) {
            should_exit = true;
            reason = "stop_loss";
        } else if (mid <= st.momentum_target) {
            should_exit = true;
            reason = "take_profit";
        } else if (st.ema_fast > st.ema_slow) {
            should_exit = true;
            reason = "ema_cross";
        }
    }

    if (should_exit) {
        bpt::common::log::info(kLog(),
                               "{} momentum exit: reason={} mid={:.2f} stop={:.2f} target={:.2f}",
                               st.symbol,
                               reason,
                               mid,
                               st.momentum_stop,
                               st.momentum_target);
        momentum_close_position(st);
    }
}

void RegimeSwitchStrategy::momentum_close_position(InstrumentState& st) {
    const int64_t net = positions_.net_qty(st.instrument_id, st.exchange_id);
    if (net == 0) {
        st.has_momentum_position = false;
        return;
    }

    const OrderSide::Value close_side = (net > 0) ? OrderSide::SELL : OrderSide::BUY;
    double price;
    if (close_side == OrderSide::BUY) {
        price = st.ask * (1.0 + aggress_bps_ / 10000.0);
    } else {
        price = st.bid * (1.0 - aggress_bps_ / 10000.0);
    }

    st.momentum_order_id = send_order(st,
                                      close_side,
                                      OrderType::LIMIT,
                                      TimeInForce::IOC,
                                      price,
                                      static_cast<double>(std::abs(net)) / kQtyScale);
}

// ── Regime Transitions ──────────────────────────────────────────────────────

void RegimeSwitchStrategy::begin_transition(InstrumentState& st, Regime target) {
    st.target_regime = target;
    st.regime = Regime::TRANSITIONING;
    st.pending_cancels.clear();
    st.close_order_id = 0;
    st.transition_start_ns = st.last_bbo_ns;

    // Cancel all grid orders.
    grid_cancel_all(st);

    // Cancel momentum order if pending.
    if (st.momentum_order_id != 0 && !st.has_momentum_position) {
        cancel_order(st, st.momentum_order_id);
        st.pending_cancels.insert(st.momentum_order_id);
        st.momentum_order_id = 0;
    }

    if (st.pending_cancels.empty()) {
        st.transition = TransitionPhase::READY;
        check_transition_complete(st);
    } else {
        st.transition = TransitionPhase::CANCELLING_ORDERS;
    }
}

void RegimeSwitchStrategy::check_transition_complete(InstrumentState& st) {
    if (st.transition == TransitionPhase::CANCELLING_ORDERS) {
        if (!st.pending_cancels.empty())
            return;
        st.transition = TransitionPhase::READY;
    }

    if (st.transition == TransitionPhase::READY) {
        const int64_t net = positions_.net_qty(st.instrument_id, st.exchange_id);

        // If we have a position and the target regime doesn't want it, close it.
        if (net != 0 && st.target_regime != Regime::MOMENTUM) {
            const OrderSide::Value close_side = (net > 0) ? OrderSide::SELL : OrderSide::BUY;
            double price;
            if (close_side == OrderSide::BUY)
                price = st.ask * (1.0 + aggress_bps_ / 10000.0);
            else
                price = st.bid * (1.0 - aggress_bps_ / 10000.0);

            st.close_order_id = send_order(st,
                                           close_side,
                                           OrderType::LIMIT,
                                           TimeInForce::IOC,
                                           price,
                                           static_cast<double>(std::abs(net)) / kQtyScale);
            st.transition = TransitionPhase::CLOSING_POSITION;
            bpt::common::log::info(kLog(),
                                   "{} closing position before entering {}",
                                   st.symbol,
                                   regime_name(st.target_regime));
            return;
        }

        // Transition complete — enter new regime.
        st.regime = st.target_regime;
        st.transition = TransitionPhase::NONE;
        st.regime_dwell = 0;
        st.has_momentum_position = (net != 0);

        bpt::common::log::info(kLog(), "{} entered regime: {}", st.symbol, regime_name(st.regime));

        if (st.regime == Regime::GRID) {
            grid_build(st);
        }
        // Momentum will pick up on next signal check.
    }
}

// ── Order Helpers ───────────────────────────────────────────────────────────

uint64_t RegimeSwitchStrategy::send_order(InstrumentState& st,
                                          bpt::messages::OrderSide::Value side,
                                          bpt::messages::OrderType::Value type,
                                          bpt::messages::TimeInForce::Value tif,
                                          double price,
                                          double qty) {
    if (!order_mgr_)
        return 0;

    bpt::common::log::info(kLog(),
                           "{} {} @ {:.2f} qty={:.6f} tif={}",
                           st.symbol,
                           side == bpt::messages::OrderSide::BUY ? "BUY" : "SELL",
                           price,
                           qty,
                           tif == bpt::messages::TimeInForce::IOC ? "IOC" : "GTC");

    const uint64_t order_id = order_mgr_->place_order(st.instrument_id, st.exchange_id, side, type, tif, price, qty);
    if (order_id == 0)
        return 0;

    order_to_instrument_[order_id] = st.instrument_id;
    bpt::common::log::info(kLog(), "order placed → order_id={}", order_id);
    return order_id;
}

void RegimeSwitchStrategy::cancel_order(InstrumentState& st, uint64_t order_id) {
    if (!order_mgr_ || order_id == 0)
        return;
    order_mgr_->cancel_order(order_id, st.exchange_id, st.instrument_id);
}

}  // namespace bpt::strategy::strategy
