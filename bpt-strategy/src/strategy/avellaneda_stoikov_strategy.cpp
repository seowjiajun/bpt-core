// AS construction + market-data inbound path: ctor/config parsing, universe
// resolution + tick handlers (book, trade, BBO → quote), toxicity feedback,
// and refdata-stale gating.
//
// Sibling translation units share this class via the header:
//   pricing / sizing / suppression → avellaneda_stoikov_quoting.cpp
//   execution-report handling      → avellaneda_stoikov_exec.cpp
//   order I/O (requote / unwind)   → avellaneda_stoikov_orders.cpp
//   state save/load + console JSON → avellaneda_stoikov_state_io.cpp
//   shutdown flatten / account     → avellaneda_stoikov_shutdown.cpp

#include "strategy/strategy/avellaneda_stoikov_strategy.h"

#include "strategy/config/fair_value_config.h"
#include "strategy/md/subscribe_helpers.h"
#include "strategy/refdata/exchange_id.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>
#include <messages/TradeSide.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <cmath>
#include <limits>

using bpt::messages::OrderSide;

namespace bpt::strategy::strategy {

namespace {
// Sub-module logger — output gets auto-prefixed with "AS" via the
// %(logger) placeholder in the default log pattern, so format strings
// don't carry the bracketed tag anymore. Lazy-initialised on first call
// because bpt::common::logging::init() runs in main(), after static init.
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("AS");
    return l;
}

}  // namespace

// ── Construction ─────────────────────────────────────────────────────────────
AvellanedaStoikovStrategy::AvellanedaStoikovStrategy(uint64_t correlation_id,
                                                     const config::StrategyConfig& cfg,
                                                     refdata::IRefdataClient& refdata,
                                                     md::IMdClient* md,
                                                     order::OrderManager* order_mgr)
    : correlation_id_(correlation_id),
      gamma_(cfg.params["gamma"].value<double>().value_or(0.1)),
      kappa_(cfg.params["kappa"].value<double>().value_or(1.5)),
      session_duration_s_(cfg.params["session_duration_s"].value<double>().value_or(86400.0)),
      vol_halflife_s_(cfg.params["vol_halflife_s"].value<double>().value_or(60.0)),
      vol_warmup_ticks_(static_cast<std::size_t>(cfg.params["vol_warmup_ticks"].value<int64_t>().value_or(20))),
      kappa_halflife_s_(cfg.params["kappa_halflife_s"].value<double>().value_or(300.0)),
      kappa_warmup_ticks_(static_cast<std::size_t>(cfg.params["kappa_warmup_ticks"].value<int64_t>().value_or(10))),
      kappa_min_(cfg.params["kappa_min"].value<double>().value_or(0.01)),
      requote_threshold_(cfg.params["requote_threshold"].value<double>().value_or(0.0001)),
      sizer_{.order_qty = cfg.params["order_qty"].value<double>().value_or(0.001),
             .order_qty_fraction = cfg.params["order_qty_fraction"].value<double>().value_or(0.0),
             .order_qty_min = cfg.params["order_qty_min"].value<double>().value_or(0.0),
             .max_inventory = cfg.params["max_inventory"].value<double>().value_or(0.1),
             .max_inventory_fraction = cfg.params["max_inventory_fraction"].value<double>().value_or(0.0)},
      min_half_spread_bps_(cfg.params["min_half_spread_bps"].value<double>().value_or(1.0)),
      max_half_spread_bps_(cfg.params["max_half_spread_bps"].value<double>().value_or(50.0)),
      quote_sanity_bps_(cfg.params["quote_sanity_bps"].value<double>().value_or(5000.0)),
      order_book_depth_(static_cast<uint8_t>(cfg.params["order_book_depth"].value<int64_t>().value_or(0))),
      fv_cfg_(config::parse_fv_config(cfg.params)),
      pause_below_rpnl_usd_(cfg.params["pause_below_rpnl_usd"].value<double>().value_or(0.0)),
      pause_cooldown_s_(cfg.params["pause_cooldown_s"].value<double>().value_or(300.0)),
      post_fill_markout_threshold_bps_(cfg.params["post_fill_markout_threshold_bps"].value<double>().value_or(0.0)),
      post_fill_markout_cooldown_s_(cfg.params["post_fill_markout_cooldown_s"].value<double>().value_or(30.0)),
      drift_halflife_s_(cfg.params["drift_halflife_s"].value<double>().value_or(30.0)),
      drift_warmup_ticks_(static_cast<std::size_t>(cfg.params["drift_warmup_ticks"].value<int64_t>().value_or(50))),
      max_drift_skew_bps_(cfg.params["max_drift_skew_bps"].value<double>().value_or(10.0)),
      slow_drift_window_s_(cfg.params["slow_drift_window_s"].value<double>().value_or(300.0)),
      supp_policy_(SuppressionPolicy::Config{
          .post_fill_markout_threshold_bps =
              cfg.params["post_fill_markout_threshold_bps"].value<double>().value_or(0.0),
          .drift_suppress_bps = cfg.params["drift_suppress_bps"].value<double>().value_or(0.0),
          .drift_suppress_sigma_mult = cfg.params["drift_suppress_sigma_mult"].value<double>().value_or(0.0),
          .slow_drift_suppress_bps = cfg.params["slow_drift_suppress_bps"].value<double>().value_or(0.0),
          .slow_drift_suppress_sigma_mult = cfg.params["slow_drift_suppress_sigma_mult"].value<double>().value_or(0.0),
          .slow_drift_window_s = cfg.params["slow_drift_window_s"].value<double>().value_or(300.0),
          .tox_suppress_threshold = cfg.params["tox_suppress_threshold"].value<double>().value_or(0.0),
          .queue_suppress_fill_prob_min = cfg.params["queue_suppress_fill_prob_min"].value<double>().value_or(0.0),
          .queue_suppress_horizon_s = cfg.params["queue_suppress_horizon_s"].value<double>().value_or(5.0),
          .kappa_min = cfg.params["kappa_min"].value<double>().value_or(0.01),
          .kappa_warmup_ticks =
              static_cast<std::size_t>(cfg.params["kappa_warmup_ticks"].value<int64_t>().value_or(10)),
          .ofi_cancel_threshold_sigma = cfg.params["ofi_cancel_threshold_sigma"].value<double>().value_or(
              std::numeric_limits<double>::infinity()),
      }),
      unwinder_(positions_,
                *order_mgr,
                {.passive_timeout_s = cfg.params["unwind_passive_timeout_s"].value<double>().value_or(45.0),
                 .step_interval_s = cfg.params["unwind_step_interval_s"].value<double>().value_or(8.0),
                 .cross_bps = cfg.params["shutdown_cross_bps"].value<double>().value_or(20.0),
                 .max_retries =
                     static_cast<uint32_t>(cfg.params["shutdown_max_unwind_retries"].value<int64_t>().value_or(3))}),
      regime_cfg_{
          cfg.params["regime_mean_revert_h"].value<double>().value_or(0.45),
          cfg.params["regime_trend_h"].value<double>().value_or(0.55),
          cfg.params["regime_hysteresis"].value<double>().value_or(0.03),
          static_cast<std::size_t>(cfg.params["regime_hurst_window"].value<int64_t>().value_or(200)),
          static_cast<std::size_t>(cfg.params["regime_warmup_samples"].value<int64_t>().value_or(50)),
          cfg.params["regime_gamma_mean_revert"].value<double>().value_or(0.6),
          cfg.params["regime_gamma_neutral"].value<double>().value_or(1.0),
          cfg.params["regime_gamma_trending"].value<double>().value_or(1.8),
          static_cast<std::size_t>(cfg.params["regime_eval_interval"].value<int64_t>().value_or(20)),
      },
      vol_gate_cfg_{
          cfg.params["vol_gate_max_bps"].value<double>().value_or(0.0),
          static_cast<uint64_t>(cfg.params["vol_gate_window_ms"].value<double>().value_or(1000.0) * 1e6),
          static_cast<uint64_t>(cfg.params["vol_gate_halt_ms"].value<double>().value_or(5000.0) * 1e6),
      },
      vol_gate_sigma_mult_(cfg.params["vol_gate_sigma_mult"].value<double>().value_or(0.0)),
      gamma_pnl_window_n_(static_cast<std::size_t>(cfg.params["gamma_pnl_window_n"].value<int64_t>().value_or(0))),
      gamma_pnl_loss_threshold_usd_(cfg.params["gamma_pnl_loss_threshold_usd"].value<double>().value_or(0.0)),
      gamma_pnl_profit_threshold_usd_(cfg.params["gamma_pnl_profit_threshold_usd"].value<double>().value_or(0.0)),
      gamma_pnl_widen_mult_(cfg.params["gamma_pnl_widen_mult"].value<double>().value_or(1.0)),
      gamma_pnl_tighten_mult_(cfg.params["gamma_pnl_tighten_mult"].value<double>().value_or(1.0)),
      ofi_weight_bps_(cfg.params["ofi_weight_bps"].value<double>().value_or(0.0)),
      ofi_window_ns_(static_cast<uint64_t>(cfg.params["ofi_window_ms"].value<double>().value_or(1000.0) * 1e6)),
      imbalance_weight_bps_(cfg.params["imbalance_weight_bps"].value<double>().value_or(0.0)),
      ofi_cancel_threshold_sigma_(
          cfg.params["ofi_cancel_threshold_sigma"].value<double>().value_or(std::numeric_limits<double>::infinity())),
      ofi_sigma_halflife_s_(cfg.params["ofi_sigma_halflife_s"].value<double>().value_or(60.0)),
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr) {
    bpt::common::log::info(kLog(),
                           "γ={:.4f} κ_fallback={:.4f} session={:.0f}s "
                           "vol_halflife={:.1f}s vol_warmup={} "
                           "kappa_halflife={:.1f}s kappa_warmup={} kappa_min={:.4f} "
                           "requote_thr={:.4f}% max_inv={:.4f} qty={:.6f} "
                           "half_spread=[{:.1f},{:.1f}]bps "
                           "drift_halflife={:.1f}s drift_suppress={:.1f}bps (σ×{:.2f}) "
                           "slow_drift_window={:.0f}s slow_drift_suppress={:.1f}bps (σ×{:.2f})",
                           gamma_,
                           kappa_,
                           session_duration_s_,
                           vol_halflife_s_,
                           vol_warmup_ticks_,
                           kappa_halflife_s_,
                           kappa_warmup_ticks_,
                           kappa_min_,
                           requote_threshold_ * 100.0,
                           sizer_.max_inventory,
                           sizer_.order_qty,
                           min_half_spread_bps_,
                           max_half_spread_bps_,
                           drift_halflife_s_,
                           supp_policy_.config().drift_suppress_bps,
                           supp_policy_.config().drift_suppress_sigma_mult,
                           slow_drift_window_s_,
                           supp_policy_.config().slow_drift_suppress_bps,
                           supp_policy_.config().slow_drift_suppress_sigma_mult);
    bpt::common::log::info(kLog(),
                           "risk: max_position_usd={} max_order_size_usd={}",
                           cfg.risk.max_position_usd,
                           cfg.risk.max_order_size_usd);
    bpt::common::log::info(kLog(),
                           "order_book_depth={} queue_suppress_fill_prob_min={:.4f}",
                           static_cast<int>(order_book_depth_),
                           supp_policy_.config().queue_suppress_fill_prob_min);
    {
        const char* fv_mode_str = "mid";
        switch (fv_cfg_.mode) {
            case FairValueEstimator::Mode::kMid:
                fv_mode_str = "mid";
                break;
            case FairValueEstimator::Mode::kMicro:
                fv_mode_str = "micro";
                break;
            case FairValueEstimator::Mode::kMicroSizeCapped:
                fv_mode_str = "micro_capped";
                break;
            case FairValueEstimator::Mode::kL2WeightedMicro:
                fv_mode_str = "l2_weighted";
                break;
            case FairValueEstimator::Mode::kEwmaMicro:
                fv_mode_str = "ewma_micro";
                break;
        }
        bpt::common::log::info(
            kLog(),
            "fair_value: mode={} size_cap={:.4f} ladder_depth={} ladder_decay={:.2f} ewma_alpha={:.2f}",
            fv_mode_str,
            fv_cfg_.size_cap_qty,
            fv_cfg_.ladder_depth,
            fv_cfg_.ladder_decay,
            fv_cfg_.ewma_alpha);
    }
    if (pause_below_rpnl_usd_ < 0.0) {
        bpt::common::log::info(kLog(),
                               "drawdown_pause: threshold_usd={:.4f} cooldown_s={:.0f}",
                               pause_below_rpnl_usd_,
                               pause_cooldown_s_);
    } else {
        bpt::common::log::info(kLog(), "drawdown_pause: disabled");
    }

    if (vol_gate_cfg_.max_bps_per_window > 0.0) {
        bpt::common::log::info(kLog(),
                               "vol_gate max_bps={:.1f} window={}ms halt={}ms",
                               vol_gate_cfg_.max_bps_per_window,
                               vol_gate_cfg_.window_ns / 1'000'000,
                               vol_gate_cfg_.halt_duration_ns / 1'000'000);
    } else {
        bpt::common::log::info(kLog(), "vol_gate disabled (vol_gate_max_bps=0)");
    }

    for (const auto& s : instruments_)
        bpt::common::log::info(kLog(), "instrument: {}", s);
}

// ── IStrategy ───────────────────────────────────────────────────────────────

void AvellanedaStoikovStrategy::start() {
    for (const auto& ex : md_exchanges_)
        bpt::common::log::info(kLog(), "MD exchange: {}", ex);

    refdata_.subscribe(correlation_id_, CanonicalResolver::build_filters(instruments_, md_exchanges_));
}

void AvellanedaStoikovStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    bpt::common::log::info(kLog(), "Snapshot ({} instruments), resolving universe...", cache.size());
    state_.clear();
    positions_.clear_all();
    // PositionTracker is now at 0; the next AccountSnapshot becomes
    // the baseline against which SPOT reconcile measures delta.
    initial_ccy_equity_e8_.clear();
    initial_ccy_equity_captured_ = false;

    for (const auto& r : CanonicalResolver::resolve_instruments(cache, instruments_, md_exchanges_)) {
        auto [it, inserted] = state_.emplace(r.instrument_id,
                                             InstrumentState{.instrument_id = r.instrument_id,
                                                             .ewma_var = EwmaVariance(vol_halflife_s_),
                                                             .ewma_drift = EwmaDrift(drift_halflife_s_),
                                                             .ewma_kappa = KappaEstimator(kappa_halflife_s_),
                                                             .symbol = r.instrument.symbol,
                                                             .exchange = r.instrument.exchange,
                                                             .exchange_id = r.exchange_id,
                                                             .instrument_type = r.instrument.type,
                                                             .base_ccy = r.instrument.base_currency,
                                                             .tick_size = r.instrument.tick_size,
                                                             .lot_size = r.instrument.lot_size,
                                                             .vol_gate = VolatilityGate(vol_gate_cfg_),
                                                             .regime = RegimeDetector(regime_cfg_)});
        if (inserted) {
            it->second.fv = FairValueEstimator{fv_cfg_};
            it->second.ofi = OFICalculator{OFICalculator::Config{
                .max_levels = static_cast<int>(order_book_depth_ > 0 ? order_book_depth_ : 5),
                .window_ns = ofi_window_ns_,
            }};
            it->second.ewma_ofi_sq = TimeWeightedEwma(ofi_sigma_halflife_s_);
        }
        bpt::common::log::info("  [{}] {} @ {} tick={} lot={}",
                               r.instrument_id,
                               r.instrument.symbol,
                               r.instrument.exchange,
                               r.instrument.tick_size,
                               r.instrument.lot_size);
    }

    bpt::common::log::info(kLog(), "Trading universe: {} instrument(s)", state_.size());

    if (!md_client_)
        return;
    auto subs = md::build_subscriptions(state_, order_book_depth_);
    bpt::common::log::info(kLog(),
                           "Subscribing MD to {} instrument(s) depth={}",
                           subs.size(),
                           static_cast<int>(order_book_depth_));
    md_client_->subscribe(correlation_id_, subs);
}

void AvellanedaStoikovStrategy::on_delta(const refdata::Instrument& inst,
                                         bpt::messages::DeltaUpdateType::Value update_type) {
    if (update_type == bpt::messages::DeltaUpdateType::ADD) {
        if (!CanonicalResolver::matches(instruments_, md_exchanges_, inst))
            return;

        const auto ex_id = refdata::to_exchange_id(inst.exchange);

        auto [it, inserted] = state_.emplace(inst.instrument_id,
                                             InstrumentState{.instrument_id = inst.instrument_id,
                                                             .ewma_var = EwmaVariance(vol_halflife_s_),
                                                             .ewma_drift = EwmaDrift(drift_halflife_s_),
                                                             .ewma_kappa = KappaEstimator(kappa_halflife_s_),
                                                             .symbol = inst.symbol,
                                                             .exchange = inst.exchange,
                                                             .exchange_id = ex_id,
                                                             .instrument_type = inst.type,
                                                             .base_ccy = inst.base_currency,
                                                             .tick_size = inst.tick_size,
                                                             .lot_size = inst.lot_size,
                                                             .vol_gate = VolatilityGate(vol_gate_cfg_),
                                                             .regime = RegimeDetector(regime_cfg_)});
        if (inserted) {
            it->second.fv = FairValueEstimator{fv_cfg_};
            it->second.ofi = OFICalculator{OFICalculator::Config{
                .max_levels = static_cast<int>(order_book_depth_ > 0 ? order_book_depth_ : 5),
                .window_ns = ofi_window_ns_,
            }};
            it->second.ewma_ofi_sq = TimeWeightedEwma(ofi_sigma_halflife_s_);
        }
        bpt::common::log::info(kLog(),
                               "Delta ADD {} @ {} tick={} lot={}",
                               inst.symbol,
                               inst.exchange,
                               inst.tick_size,
                               inst.lot_size);

    } else if (update_type == bpt::messages::DeltaUpdateType::REMOVE) {
        state_.erase(inst.instrument_id);
        bpt::common::log::info(kLog(), "Delta REMOVE {} @ {}", inst.symbol, inst.exchange);
    }
}

void AvellanedaStoikovStrategy::on_order_book(const bpt::messages::MdOrderBook& book) {
    auto it = state_.find(book.instrumentId());
    if (it == state_.end())
        return;
    InstrumentState& st = it->second;
    // With order_book_depth_ <= 5 we're on OKX `books5` (or equivalent
    // snapshot-only channels), where every frame is a full top-N
    // snapshot — clear+rebuild avoids stale-level accumulation. Above 5
    // we're on a delta channel and fold-only is correct.
    const bool is_snapshot = order_book_depth_ <= 5;
    st.book.apply(book, is_snapshot);

    // Feed the maintained ladder into the OFI rolling estimator —
    // ONLY when the signal is actually enabled. Even unused, the
    // top_bids/top_asks copy + deque maintenance perturbs strategy
    // timing enough to shift fill outcomes through the Aeron-IPC
    // simulator, breaking the "ofi_weight_bps=0 is identical to the
    // prior baseline" property required for the calibration grid.
    //
    // Hot-path allocation note: the four scratch buffers live on
    // InstrumentState so subsequent ticks .clear() + refill instead of
    // constructing fresh vectors. First tick after warmup grows them
    // to K levels; from there on the per-tick cost is purely
    // amortised iteration.
    // OFI update gate widened: also fires when the cancel rule is armed.
    // Keep the "skip when fully off" property so existing baselines stay
    // byte-identical when both knobs are disabled (weight=0, threshold=inf).
    const bool cancel_armed = !std::isinf(ofi_cancel_threshold_sigma_);
    const bool ofi_active = ofi_weight_bps_ != 0.0 || cancel_armed;
    if (ofi_active && st.book.ready()) {
        const std::size_t K = order_book_depth_ > 0 ? order_book_depth_ : 5;
        st.book.top_bids(K, st.ofi_top_bid_buf);
        st.book.top_asks(K, st.ofi_top_ask_buf);
        st.ofi_bids_buf.clear();
        st.ofi_asks_buf.clear();
        for (const auto& l : st.ofi_top_bid_buf)
            st.ofi_bids_buf.emplace_back(l.price, l.qty);
        for (const auto& l : st.ofi_top_ask_buf)
            st.ofi_asks_buf.emplace_back(l.price, l.qty);
        st.ofi.update(st.ofi_bids_buf, st.ofi_asks_buf, book.timestampNs());

        // Rolling σ²(OFI) — only updated when the cancel rule is armed.
        // Skipping when only the skew is active keeps the legacy
        // ofi_weight_bps-only hot path identical.
        if (cancel_armed) {
            const double v = st.ofi.value();
            const double dt_s =
                st.ewma_ofi_last_ns > 0 ? static_cast<double>(book.timestampNs() - st.ewma_ofi_last_ns) * 1e-9 : 0.0;
            st.ewma_ofi_sq.update(v * v, dt_s);
            st.ewma_ofi_last_ns = book.timestampNs();
        }
    }

    // Diagnostic: log maintained-ladder state periodically so we can
    // confirm deltas are merging correctly. The raw per-frame delta
    // counts are unhelpful (OKX sends 2-8 levels per message); what
    // matters is that the folded ladder has real depth.
    static uint64_t ob_count = 0;
    ++ob_count;
    if (ob_count > 5 && ob_count % 500 != 0)
        return;
    if (!st.book.ready())
        return;
    bpt::common::log::info(kLog(),
                           "Book #{}: id={} ladder bids={} asks={} "
                           "best_bid={:.4f}@{:.6f} best_ask={:.4f}@{:.6f} mid={:.4f}",
                           ob_count,
                           book.instrumentId(),
                           st.book.n_bid_levels(),
                           st.book.n_ask_levels(),
                           st.book.best_bid(),
                           st.book.best_bid_qty(),
                           st.book.best_ask(),
                           st.book.best_ask_qty(),
                           st.book.mid());
}

void AvellanedaStoikovStrategy::on_trade(const bpt::messages::MdTrade& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;

    InstrumentState& st = it->second;
    const uint64_t ts_ns = tick.timestampNs();

    // Queue-position decrement: a trade hitting the passive side of our
    // price level shrinks queue_ahead for any of our resting orders at
    // that price. TradeSide carries the aggressor side — same BUY/SELL
    // values as OrderSide.
    using bpt::messages::TradeSide;
    const OrderSide::Value aggressor = (tick.side() == TradeSide::BUY) ? OrderSide::BUY : OrderSide::SELL;
    st.queue.on_trade(aggressor, tick.price(), static_cast<double>(tick.qty()) / 1e8, ts_ns);

    st.ewma_kappa.update(ts_ns);
    st.last_trade_ns = ts_ns;
}

void AvellanedaStoikovStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    // Refdata heartbeat stale → fee_cache.get() will return nullopt and
    // quotes would ship with zero fee buffer. Skip the entire tick path;
    // existing cancels (issued by on_refdata_stale_changed) keep flowing
    // through on_exec_report. EWMA estimators see a gap during the pause
    // window — accepted; warmup re-bootstraps quickly when refdata returns.
    if (refdata_stale_)
        return;

    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;

    InstrumentState& st = it->second;

    const double bid_px = tick.bidPrice();
    const double ask_px = tick.askPrice();
    if (bid_px <= 0.0 || ask_px <= 0.0 || ask_px <= bid_px)
        return;

    const double mid = (bid_px + ask_px) * 0.5;
    const uint64_t ts_ns = tick.timestampNs();

    // Cache market top-of-book for the console overlay. Done here (not
    // just before publish) so the strategy sees the same values the
    // console does, and works even when order_book_depth=0 leaves
    // st.book unpopulated.
    st.last_market_bid = bid_px;
    st.last_market_ask = ask_px;

    // Phase 2.1 — evaluate post-fill markout on the first BBO tick
    // after a fill. Convention: positive markout = favorable for us
    // (BUY → mid moved up; SELL → mid moved down). If markout breaches
    // the threshold, suspend that side for the configured cooldown
    // window. Cooldown timestamps are in simulation time (ts_ns) so
    // they translate consistently between live and backtest.
    if (st.pending_buy_fill_price > 0.0) {
        const double markout_bps = (mid - st.pending_buy_fill_price) / st.pending_buy_fill_price * 1e4;
        if (markout_bps < post_fill_markout_threshold_bps_) {
            const uint64_t cooldown_ns = static_cast<uint64_t>(post_fill_markout_cooldown_s_ * 1e9);
            st.post_fill_suspend_until_bid = ts_ns + cooldown_ns;
            bpt::common::log::warn(kLog(),
                                   "{} post-fill BUY markout {:.2f} bps < {:.2f} — suspending bid for {:.1f}s",
                                   st.symbol,
                                   markout_bps,
                                   post_fill_markout_threshold_bps_,
                                   post_fill_markout_cooldown_s_);
        }
        st.pending_buy_fill_price = 0.0;
    }
    if (st.pending_sell_fill_price > 0.0) {
        // SELL favorable = mid moved DOWN, so flip sign vs BUY case.
        const double markout_bps = (st.pending_sell_fill_price - mid) / st.pending_sell_fill_price * 1e4;
        if (markout_bps < post_fill_markout_threshold_bps_) {
            const uint64_t cooldown_ns = static_cast<uint64_t>(post_fill_markout_cooldown_s_ * 1e9);
            st.post_fill_suspend_until_ask = ts_ns + cooldown_ns;
            bpt::common::log::warn(kLog(),
                                   "{} post-fill SELL markout {:.2f} bps < {:.2f} — suspending ask for {:.1f}s",
                                   st.symbol,
                                   markout_bps,
                                   post_fill_markout_threshold_bps_,
                                   post_fill_markout_cooldown_s_);
        }
        st.pending_sell_fill_price = 0.0;
    }

    // Slow-drift anchor. Seeded on first tick, advanced once per
    // window duration. slow_drift_bps expresses cumulative return
    // from the anchor in bps — see the trend-suppression block in
    // compute_quotes for how it drives side cutoff.
    if (st.slow_drift_window_start_ns == 0) {
        st.slow_drift_window_start_mid = mid;
        st.slow_drift_window_start_ns = ts_ns;
    } else {
        const uint64_t window_ns = static_cast<uint64_t>(slow_drift_window_s_ * 1e9);
        if (ts_ns - st.slow_drift_window_start_ns > window_ns) {
            st.slow_drift_window_start_mid = mid;
            st.slow_drift_window_start_ns = ts_ns;
        }
    }
    if (st.slow_drift_window_start_mid > 0.0) {
        st.slow_drift_bps = (mid - st.slow_drift_window_start_mid) / st.slow_drift_window_start_mid * 1e4;
    }

    // ── Update EWMA volatility ─────────────────────────────────────────────
    if (st.session_start_ns == 0)
        st.session_start_ns = ts_ns;

    st.ewma_var.update(mid, ts_ns);
    st.ewma_drift.update(mid, ts_ns);

    // Periodic drift diagnostic — log every 20 ticks so we can see
    // what values µ reaches without turning on full debug logging.
    if (st.ewma_var.count() > 0 && st.ewma_var.count() % 20 == 0) {
        bpt::common::log::info(kLog(),
                               "{} drift µ={:.4f} ({:.1f}bps/√s) σ²={:.2e} ticks={}",
                               st.symbol,
                               st.ewma_drift.value(),
                               std::abs(st.ewma_drift.value()) * 1e4,
                               st.ewma_var.value(),
                               st.ewma_var.count());
    }
    st.last_mid = mid;
    st.last_tick_ns = ts_ns;

    // ── Regime detector ───────────────────────────────────────────────────
    st.regime.update(mid);

    // ── Volatility gate ────────────────────────────────────────────────────
    // Feed the gate on every tick so its rolling window stays current,
    // even if we're not going to quote this tick. A trip here means a
    // fast move just happened; we cancel live quotes (so we don't get
    // run over on stale depth) and skip re-quoting until the halt
    // cooldown expires.
    //
    // Adaptive threshold: if vol_gate_sigma_mult_ > 0 and the vol EWMA
    // has warmed, push `max(fixed_floor, k × σ × √window_s)` into the
    // gate before the update. Lets one `vol_gate_sigma_mult` value
    // work across asset classes — threshold tracks the realized vol
    // instead of needing per-venue bps tuning.
    if (vol_gate_sigma_mult_ > 0.0 && st.ewma_var.value() > 0.0) {
        const double sigma_bps = std::sqrt(st.ewma_var.value()) * 1e4;
        const double window_s = static_cast<double>(vol_gate_cfg_.window_ns) * 1e-9;
        const double adaptive_bps = vol_gate_sigma_mult_ * sigma_bps * std::sqrt(window_s);
        st.vol_gate.set_max_bps_per_window(std::max(vol_gate_cfg_.max_bps_per_window, adaptive_bps));
    }
    const bool was_halted = st.vol_gate.is_halted(ts_ns);
    const bool now_halted = st.vol_gate.update_and_check(mid, ts_ns);
    if (now_halted && !was_halted) {
        bpt::common::log::warn(kLog(),
                               "{} VOL HALT tripped last_trip={:.1f}bps — cancelling live quotes, pausing for {}ms",
                               st.symbol,
                               st.vol_gate.last_trip_bps(),
                               vol_gate_cfg_.halt_duration_ns / 1'000'000);
        if (order_mgr_) {
            if (st.h_bid.live())
                order_mgr_->send_cancel(st.h_bid);
            if (st.h_ask.live())
                order_mgr_->send_cancel(st.h_ask);
        }
    } else if (was_halted && !now_halted) {
        bpt::common::log::info(kLog(), "{} vol halt cleared — quoting re-enabled", st.symbol);
    }
    if (now_halted)
        return;  // don't compute or place new quotes while halted

    // ── Compute AS quotes ──────────────────────────────────────────────────
    // OrderGateway encodes quantity at 1e8 fixed-point (same scale as price).
    // Divide by 1e8 to convert raw position to base units (BTC).
    const double net_qty = static_cast<double>(positions_.net_qty(tick.instrumentId(), st.exchange_id)) / 1e8;

    // AS reservation-price reference. Estimator may bias toward micro
    // (size-weighted), L2-weighted, or EWMA — see [fair_value] config.
    // EWMA σ², drift µ, slow-drift anchor, and st.last_mid all stay on
    // raw mid above; only the `s` consumed by compute_quotes shifts.
    // Fall back to mid if the estimator returns NaN (degenerate quote).
    const double s_est = st.fv.estimate(bid_px, ask_px, tick.bidQty(), tick.askQty());
    const double s = std::isnan(s_est) ? mid : s_est;

    const BboContext ctx{.net_qty = net_qty, .mid = s, .ts_ns = ts_ns};
    const auto quotes = compute_quotes(st, ctx);
    if (!quotes)
        return;
    maybe_requote(st, ctx, *quotes);
}

// ── Toxicity feedback from Analytics ──────────────────────────────────────────────

void AvellanedaStoikovStrategy::on_toxicity_update(const bpt::analytics::messaging::ToxicityUpdate& update) {
    auto it = state_.find(update.instrument_id);
    if (it == state_.end())
        return;

    auto& st = it->second;
    st.tox_bid_toxicity = update.bid_toxicity_score;
    st.tox_ask_toxicity = update.ask_toxicity_score;
    st.tox_data_received = true;

    double bid_score = update.bid_toxicity_score;
    double ask_score = update.ask_toxicity_score;
    uint32_t bid_n = update.bid_sample_count;
    uint32_t ask_n = update.ask_sample_count;
    bpt::common::log::info(kLog(),
                           "{} Analytics update: bid_tox={:.2f}(n={}) ask_tox={:.2f}(n={})",
                           st.symbol,
                           bid_score,
                           bid_n,
                           ask_score,
                           ask_n);
}

void AvellanedaStoikovStrategy::on_refdata_stale_changed(bool stale) {
    if (stale == refdata_stale_)
        return;  // idempotent — only act on edge

    refdata_stale_ = stale;

    if (stale) {
        bpt::common::log::warn(kLog(), "Refdata heartbeat stale — pausing new quotes, cancelling resting orders");
        // Cancel every live bid/ask. Without this, existing orders keep
        // filling at prices computed before the pause — which is exactly
        // the silent-bleed failure mode this gate is designed to prevent.
        // Same pattern as the vol_halted branch in on_bbo.
        if (!order_mgr_)
            return;
        for (auto& [inst_id, st] : state_) {
            (void)inst_id;
            if (st.h_bid.live())
                order_mgr_->send_cancel(st.h_bid);
            if (st.h_ask.live())
                order_mgr_->send_cancel(st.h_ask);
        }
    } else {
        bpt::common::log::info(kLog(), "Refdata heartbeat resumed — quoting re-enabled");
    }
}

}  // namespace bpt::strategy::strategy
