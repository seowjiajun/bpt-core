#include "strategy/strategy/avellaneda_stoikov_strategy.h"

#include "strategy/clock/sim_clock.h"
#include "strategy/md/subscribe_helpers.h"
#include "strategy/refdata/exchange_id.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/InstrumentType.h>
#include <messages/OrderType.h>
#include <messages/RejectSource.h>
#include <messages/TimeInForce.h>
#include <messages/TradeSide.h>
#include <messages/exec_inst.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <cmath>

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectSource;
using bpt::messages::TimeInForce;

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

FairValueEstimator::Config parse_fair_value_config(const toml::table& params) {
    FairValueEstimator::Config c;
    auto fv = params["fair_value"];
    if (!fv)
        return c;  // No table → keep defaults (Mode::kMid).
    const std::string mode = fv["mode"].value<std::string>().value_or("mid");
    if (mode == "mid")
        c.mode = FairValueEstimator::Mode::kMid;
    else if (mode == "micro")
        c.mode = FairValueEstimator::Mode::kMicro;
    else if (mode == "micro_capped")
        c.mode = FairValueEstimator::Mode::kMicroSizeCapped;
    else if (mode == "l2_weighted")
        c.mode = FairValueEstimator::Mode::kL2WeightedMicro;
    else if (mode == "ewma_micro")
        c.mode = FairValueEstimator::Mode::kEwmaMicro;
    else
        bpt::common::log::warn(kLog(), "[fair_value] unknown mode='{}' — falling back to 'mid'", mode);
    c.size_cap_qty = fv["size_cap_qty"].value<double>().value_or(c.size_cap_qty);
    c.ladder_depth =
        static_cast<std::size_t>(fv["ladder_depth"].value<int64_t>().value_or(static_cast<int64_t>(c.ladder_depth)));
    c.ladder_decay = fv["ladder_decay"].value<double>().value_or(c.ladder_decay);
    c.ewma_alpha = fv["ewma_alpha"].value<double>().value_or(c.ewma_alpha);
    return c;
}
}  // namespace

static constexpr double kPriceScale = 1e8;

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
      max_inventory_(cfg.params["max_inventory"].value<double>().value_or(0.1)),
      order_qty_(cfg.params["order_qty"].value<double>().value_or(0.001)),
      order_qty_fraction_(cfg.params["order_qty_fraction"].value<double>().value_or(0.0)),
      order_qty_min_(cfg.params["order_qty_min"].value<double>().value_or(0.0)),
      max_inventory_fraction_(cfg.params["max_inventory_fraction"].value<double>().value_or(0.0)),
      min_half_spread_bps_(cfg.params["min_half_spread_bps"].value<double>().value_or(1.0)),
      max_half_spread_bps_(cfg.params["max_half_spread_bps"].value<double>().value_or(50.0)),
      quote_sanity_bps_(cfg.params["quote_sanity_bps"].value<double>().value_or(5000.0)),
      order_book_depth_(static_cast<uint8_t>(cfg.params["order_book_depth"].value<int64_t>().value_or(0))),
      fv_cfg_(parse_fair_value_config(cfg.params)),
      pause_below_rpnl_usd_(cfg.params["pause_below_rpnl_usd"].value<double>().value_or(0.0)),
      pause_cooldown_s_(cfg.params["pause_cooldown_s"].value<double>().value_or(300.0)),
      post_fill_markout_threshold_bps_(cfg.params["post_fill_markout_threshold_bps"].value<double>().value_or(0.0)),
      post_fill_markout_cooldown_s_(cfg.params["post_fill_markout_cooldown_s"].value<double>().value_or(30.0)),
      drift_halflife_s_(cfg.params["drift_halflife_s"].value<double>().value_or(30.0)),
      drift_warmup_ticks_(static_cast<std::size_t>(cfg.params["drift_warmup_ticks"].value<int64_t>().value_or(50))),
      max_drift_skew_bps_(cfg.params["max_drift_skew_bps"].value<double>().value_or(10.0)),
      drift_suppress_bps_(cfg.params["drift_suppress_bps"].value<double>().value_or(0.0)),
      drift_suppress_sigma_mult_(cfg.params["drift_suppress_sigma_mult"].value<double>().value_or(0.0)),
      slow_drift_window_s_(cfg.params["slow_drift_window_s"].value<double>().value_or(300.0)),
      slow_drift_suppress_bps_(cfg.params["slow_drift_suppress_bps"].value<double>().value_or(0.0)),
      slow_drift_suppress_sigma_mult_(cfg.params["slow_drift_suppress_sigma_mult"].value<double>().value_or(0.0)),
      tox_suppress_threshold_(cfg.params["tox_suppress_threshold"].value<double>().value_or(0.0)),
      queue_suppress_fill_prob_min_(cfg.params["queue_suppress_fill_prob_min"].value<double>().value_or(0.0)),
      shutdown_cross_bps_(cfg.params["shutdown_cross_bps"].value<double>().value_or(20.0)),
      shutdown_max_unwind_retries_(
          static_cast<uint32_t>(cfg.params["shutdown_max_unwind_retries"].value<int64_t>().value_or(3))),
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
                           max_inventory_,
                           order_qty_,
                           min_half_spread_bps_,
                           max_half_spread_bps_,
                           drift_halflife_s_,
                           drift_suppress_bps_,
                           drift_suppress_sigma_mult_,
                           slow_drift_window_s_,
                           slow_drift_suppress_bps_,
                           slow_drift_suppress_sigma_mult_);
    bpt::common::log::info(kLog(),
                           "risk: max_position_usd={} max_order_size_usd={}",
                           cfg.risk.max_position_usd,
                           cfg.risk.max_order_size_usd);
    bpt::common::log::info(kLog(),
                           "order_book_depth={} queue_suppress_fill_prob_min={:.4f}",
                           static_cast<int>(order_book_depth_),
                           queue_suppress_fill_prob_min_);
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
    order_to_instrument_.clear();
    positions_.clear_all();
    // PositionTracker is now at 0; the next AccountSnapshot becomes
    // the baseline against which SPOT reconcile measures delta.
    initial_ccy_equity_e8_.clear();
    initial_ccy_equity_captured_ = false;

    for (const auto& r : CanonicalResolver::resolve_instruments(cache, instruments_, md_exchanges_)) {
        auto [it, inserted] = state_.emplace(r.instrument_id,
                                             InstrumentState{.symbol = r.instrument.symbol,
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
                                             InstrumentState{.symbol = inst.symbol,
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
    if (ofi_weight_bps_ != 0.0 && st.book.ready()) {
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

    if (st.last_trade_ns > 0 && ts_ns > st.last_trade_ns) {
        const double dt_s = static_cast<double>(ts_ns - st.last_trade_ns) * 1e-9;
        if (dt_s > 0.0) {
            // Instantaneous arrival rate for this side = 1/dt_s.
            // Divide by 2 to split across bid and ask sides (each side gets half
            // the total trade flow in a symmetric market).
            const double arrival_rate = 0.5 / dt_s;
            const double lambda = std::exp(-dt_s / kappa_halflife_s_);
            st.ewma_kappa = lambda * st.ewma_kappa + (1.0 - lambda) * arrival_rate;
            ++st.kappa_ticks;
        }
    }
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

    if (st.last_mid > 0.0 && ts_ns > st.last_tick_ns) {
        const double dt_s = static_cast<double>(ts_ns - st.last_tick_ns) * 1e-9;
        if (dt_s > 0.0) {
            const double log_ret = std::log(mid / st.last_mid);
            const double norm_ret = log_ret / std::sqrt(dt_s);  // per-sqrt-second units
            const double norm_ret_sq = norm_ret * norm_ret;

            // λ = exp(-dt_s / halflife) — recomputed per tick so the decay rate is
            // proportional to elapsed time, not tick count. This makes the EWMA
            // time-consistent regardless of tick arrival rate.
            const double lambda = std::exp(-dt_s / vol_halflife_s_);
            st.ewma_var = lambda * st.ewma_var + (1.0 - lambda) * norm_ret_sq;
            ++st.ewma_ticks;

            // Drift (µ) — same EWMA of signed returns, separate halflife.
            // Shorter halflife lets µ react faster to regime changes than σ².
            const double drift_lambda = std::exp(-dt_s / drift_halflife_s_);
            st.ewma_drift = drift_lambda * st.ewma_drift + (1.0 - drift_lambda) * norm_ret;
            ++st.drift_ticks;

            // Periodic drift diagnostic — log every 100 ticks so we can see
            // what values µ reaches without turning on full debug logging.
            if (st.ewma_ticks % 20 == 0) {
                bpt::common::log::info(kLog(),
                                       "{} drift µ={:.4f} ({:.1f}bps/√s) σ²={:.2e} ticks={}",
                                       st.symbol,
                                       st.ewma_drift,
                                       std::abs(st.ewma_drift) * 1e4,
                                       st.ewma_var,
                                       st.ewma_ticks);
            }
        }
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
    if (vol_gate_sigma_mult_ > 0.0 && st.ewma_var > 0.0) {
        const double sigma_bps = std::sqrt(st.ewma_var) * 1e4;
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
            if (st.bid_order_id != 0 && !st.bid_cancel_pending) {
                order_mgr_->send_cancel(order::CancelOrderRequest{st.bid_order_id, st.exchange_id, tick.instrumentId()});
                st.bid_cancel_pending = true;
            }
            if (st.ask_order_id != 0 && !st.ask_cancel_pending) {
                order_mgr_->send_cancel(order::CancelOrderRequest{st.ask_order_id, st.exchange_id, tick.instrumentId()});
                st.ask_cancel_pending = true;
            }
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

    double new_bid{0.0}, new_ask{0.0};
    if (!compute_quotes(st, tick.instrumentId(), net_qty, s, ts_ns, new_bid, new_ask))
        return;

    maybe_requote(tick.instrumentId(), st, net_qty, s, new_bid, new_ask);
}

void AvellanedaStoikovStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const uint64_t order_id = rpt.orderId();
    const uint64_t instrument_id = rpt.instrumentId();
    const auto status = rpt.status();

    auto inst_it = order_to_instrument_.find(order_id);
    if (inst_it == order_to_instrument_.end())
        return;

    // Use the canonical instrument_id we stored at order placement — the exec
    // report's instrumentId() may be 0 if the gateway doesn't carry canonical IDs.
    const uint64_t canonical_id = inst_it->second;
    auto state_it = state_.find(canonical_id);
    if (state_it == state_.end())
        return;

    InstrumentState& st = state_it->second;

    if (status == ExecStatus::ACKED) {
        bpt::common::log::debug(kLog(), "ExecReport order_id={} {} {} ACKED", order_id, st.symbol, st.exchange);
    } else if (status == ExecStatus::REJECTED) {
        const auto src = rpt.rejectSource();
        const bool gateway_reject = (src == RejectSource::GATEWAY || src == RejectSource::RISK);
        if (gateway_reject)
            bpt::common::log::error(kLog(),
                                    "ExecReport order_id={} {} {} REJECTED reason={} source={}",
                                    order_id,
                                    st.symbol,
                                    st.exchange,
                                    bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                    bpt::messages::RejectSource::c_str(src));
        else
            bpt::common::log::warn(kLog(),
                                   "ExecReport order_id={} {} {} REJECTED reason={} source={}",
                                   order_id,
                                   st.symbol,
                                   st.exchange,
                                   bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                   bpt::messages::RejectSource::c_str(src));
    } else {
        bpt::common::log::info(kLog(),
                               "ExecReport order_id={} {} {} status={} filled={:.6f} price={:.2f}",
                               order_id,
                               st.symbol,
                               st.exchange,
                               bpt::messages::ExecStatus::c_str(status),
                               static_cast<double>(rpt.filledQty()) / 1e8,
                               static_cast<double>(rpt.price()) / 1e8);
    }

    if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
        // Capture rpnl baseline before the fill so we can derive this
        // fill's CONTRIBUTION to realized PnL (PositionTracker holds
        // session-cumulative; γ-feedback wants per-fill delta). When
        // there's no prior position, baseline = 0.
        const auto before = positions_.get(canonical_id, st.exchange_id);
        const double prior_rpnl = before ? before->realized_pnl : 0.0;

        positions_.on_fill(canonical_id, st.exchange_id, rpt.side(), rpt.filledQty(), rpt.price());
        st.queue.on_fill(order_id, static_cast<double>(rpt.filledQty()) / 1e8);

        // Phase 2.1 — record the fill for post-fill markout evaluation
        // on the next BBO tick. Skip when the feature is disabled
        // (threshold == 0). Excludes unwind orders: those are
        // intentionally aggressive and we don't want their adverse
        // markout to trip the cooldown for the passive side.
        // Timestamps are simulation time (st.last_tick_ns), not wall
        // clock — backtest replays compress 11h of sim time into a few
        // seconds of wall clock, so a wall-clock cooldown would span
        // the whole run.
        if (post_fill_markout_threshold_bps_ < 0.0 && order_id != st.unwind_order_id) {
            const double fill_px = static_cast<double>(rpt.price()) / 1e8;
            if (rpt.side() == bpt::messages::TradeSide::BUY) {
                st.pending_buy_fill_price = fill_px;
                st.pending_buy_fill_ts = st.last_tick_ns;
            } else {
                st.pending_sell_fill_price = fill_px;
                st.pending_sell_fill_ts = st.last_tick_ns;
            }
        }

        if (const auto pos = positions_.get(canonical_id, st.exchange_id)) {
            bpt::common::log::info(kLog(),
                                   "Position {} @ {}  net_qty={:.6f}  avg_price={:.2f}  rpnl={:.4f}",
                                   st.symbol,
                                   st.exchange,
                                   static_cast<double>(pos->net_qty) / 1e8,
                                   pos->avg_price,
                                   pos->realized_pnl);

            // γ-feedback rolling window — only push when feature enabled
            // and the fill actually realized PnL (opening fills don't
            // realize anything; deltas of 0 would dilute the window).
            if (gamma_pnl_window_n_ > 0) {
                const double delta = pos->realized_pnl - prior_rpnl;
                if (delta != 0.0) {
                    st.recent_rpnl.push_back(delta);
                    while (st.recent_rpnl.size() > gamma_pnl_window_n_)
                        st.recent_rpnl.pop_front();
                }
            }

            // Drawdown circuit-breaker. Trigger when realized PnL crosses
            // the configured loss threshold AND we're not already in a
            // pause window. The pause_active flag in SuppressionState
            // prevents NEW quotes; we ALSO have to actively cancel any
            // resting bid/ask here — otherwise pre-existing live orders
            // sit in the book and keep filling during the pause window.
            // (Same pattern as the vol_halted cancel block in on_bbo:
            // suppression alone doesn't pull live orders, only stops
            // requotes; explicit cancel is required.)
            if (pause_below_rpnl_usd_ < 0.0 && pos->realized_pnl < pause_below_rpnl_usd_ &&
                prior_rpnl >= pause_below_rpnl_usd_) {
                const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
                st.pause_until_ns = now_ns + static_cast<uint64_t>(pause_cooldown_s_ * 1e9);
                bpt::common::log::warn(kLog(),
                                       "{} PAUSE TRIGGERED rpnl={:.4f} crossed below threshold={:.4f} — "
                                       "halting both sides for {:.0f}s",
                                       st.symbol,
                                       pos->realized_pnl,
                                       pause_below_rpnl_usd_,
                                       pause_cooldown_s_);
                if (order_mgr_) {
                    if (st.bid_order_id != 0 && !st.bid_cancel_pending) {
                        order_mgr_->send_cancel(order::CancelOrderRequest{st.bid_order_id, st.exchange_id, canonical_id});
                        st.bid_cancel_pending = true;
                    }
                    if (st.ask_order_id != 0 && !st.ask_cancel_pending) {
                        order_mgr_->send_cancel(order::CancelOrderRequest{st.ask_order_id, st.exchange_id, canonical_id});
                        st.ask_cancel_pending = true;
                    }
                }
            }
        }
    }

    // Terminal statuses: clear order slot and cancel-pending flags.
    bool was_unwind_terminal = false;
    if (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED) {
        st.queue.on_cancel(order_id);
        if (st.bid_order_id == order_id) {
            st.bid_order_id = 0;
            st.bid_cancel_pending = false;
        } else if (st.ask_order_id == order_id) {
            st.ask_order_id = 0;
            st.ask_cancel_pending = false;
        } else if (st.unwind_order_id == order_id) {
            st.unwind_order_id = 0;
            was_unwind_terminal = true;
        }
        order_to_instrument_.erase(order_id);
    }
    // PARTIAL: order still live — keep order_id and pending flags unchanged.
    // ACKED:   acknowledged but not yet filled — keep order_id.

    // Shutdown retry: if this terminal was an unwind and residual
    // position remains (reject with no fill, or partial+IOC-cancel),
    // re-fire against a fresh mid while the retry budget is non-zero.
    // Budget is armed only by on_shutdown_flatten(); normal-path
    // inventory unwinds don't enter this branch.
    if (was_unwind_terminal && st.unwind_retries_left > 0) {
        const int64_t net_qty_e8 = positions_.net_qty(canonical_id, st.exchange_id);
        if (net_qty_e8 != 0 && st.last_mid > 0.0) {
            const double residual = static_cast<double>(std::abs(net_qty_e8)) / 1e8;
            const auto retry_side = (net_qty_e8 > 0) ? OrderSide::SELL : OrderSide::BUY;
            --st.unwind_retries_left;
            bpt::common::log::warn(kLog(),
                                   "SHUTDOWN RETRY {} {} residual_qty={:.8f} retries_left={}",
                                   st.symbol,
                                   st.exchange,
                                   residual,
                                   st.unwind_retries_left);
            send_unwind_order(canonical_id, st, retry_side, st.last_mid, residual);
        } else {
            // Flat — clear budget so has_pending_flatten() settles.
            st.unwind_retries_left = 0;
        }
    } else if (was_unwind_terminal && st.unwind_retries_left == 0 && st.unwind_is_shutdown_drain) {
        const int64_t net_qty_e8 = positions_.net_qty(canonical_id, st.exchange_id);
        if (net_qty_e8 != 0) {
            bpt::common::log::error(
                kLog(),
                "SHUTDOWN RETRIES EXHAUSTED {} {} residual_qty={:.8f} — position leaking past drain",
                st.symbol,
                st.exchange,
                static_cast<double>(std::abs(net_qty_e8)) / 1e8);
        }
    }
    if (was_unwind_terminal)
        st.unwind_is_shutdown_drain = false;

    // Exchange-error backoff: consecutive EXCHANGE-sourced rejections trigger
    // increasing cooldowns so we don't flood a broken/unfunded account.
    if (status == ExecStatus::REJECTED && rpt.rejectSource() == RejectSource::EXCHANGE) {
        ++st.consecutive_exchange_errors;
        const uint64_t backoff_s = (st.consecutive_exchange_errors == 1)   ? 5
                                   : (st.consecutive_exchange_errors == 2) ? 15
                                                                           : 30;
        const uint64_t now_ns = bpt::strategy::clock::SimClock::now_ns();
        st.reject_backoff_until_ns = now_ns + backoff_s * 1'000'000'000ULL;
        bpt::common::log::warn(kLog(),
                               "Exchange rejection backoff {} @ {}: {}s (consecutive={})",
                               st.symbol,
                               st.exchange,
                               backoff_s,
                               st.consecutive_exchange_errors);
    } else if (status == ExecStatus::ACKED) {
        st.consecutive_exchange_errors = 0;
        st.reject_backoff_until_ns = 0;
    }
}

// ── Private ─────────────────────────────────────────────────────────────────
// compute_quotes / effective_order_qty / effective_max_inventory /
// gamma_pnl_mult / compute_suppression → avellaneda_stoikov_quoting.cpp


void AvellanedaStoikovStrategy::maybe_requote(uint64_t instrument_id,
                                              InstrumentState& st,
                                              double net_qty,
                                              double mid,
                                              double new_bid,
                                              double new_ask) {
    // Honour exchange-error backoff before touching orders on this instrument.
    if (st.reject_backoff_until_ns > 0) {
        const uint64_t now_ns = bpt::strategy::clock::SimClock::now_ns();
        if (now_ns < st.reject_backoff_until_ns)
            return;
        // Backoff expired — clear it and allow quoting to resume.
        st.reject_backoff_until_ns = 0;
        bpt::common::log::info(kLog(), "Exchange backoff expired for {} @ {}, resuming quotes", st.symbol, st.exchange);
    }

    const SuppressionState supp = compute_suppression(st, net_qty, new_bid, new_ask);

    // Resolve per-tick sizing — adaptive when order_qty_fraction_ > 0,
    // fixed otherwise. Computed once so every order submit / modify /
    // unwind this tick uses the same qty (important: if equity /
    // price change between calls, downstream aggregation gets messy).
    const double eff_qty = effective_order_qty(st);

    // Info-level logging of the runtime triggers. Console reporting
    // consumes the same supp struct via get_strategy_state_json, so
    // these log lines and the rendered badge can't drift.
    if (supp.trend_bid || supp.trend_ask) {
        bpt::common::log::info(kLog(),
                               "{} trend suppress |Δ|={:.1f}bps > {:.1f}bps over {:.0f}s window — suppressing {}",
                               st.symbol,
                               std::abs(st.slow_drift_bps),
                               slow_drift_suppress_bps_,
                               slow_drift_window_s_,
                               supp.trend_ask ? "asks" : "bids");
    }
    if (supp.drift_bid || supp.drift_ask) {
        bpt::common::log::info(kLog(),
                               "{} drift suppress |µ|={:.1f}bps > {:.1f}bps — suppressing {}",
                               st.symbol,
                               std::abs(st.ewma_drift) * 1e4,
                               drift_suppress_bps_,
                               supp.drift_ask ? "asks" : "bids");
    }
    if (supp.tox_bid) {
        bpt::common::log::info(kLog(),
                               "{} tox suppress bids: score={:.2f} < {:.2f}",
                               st.symbol,
                               st.tox_bid_toxicity,
                               tox_suppress_threshold_);
    }
    if (supp.tox_ask) {
        bpt::common::log::info(kLog(),
                               "{} tox suppress asks: score={:.2f} < {:.2f}",
                               st.symbol,
                               st.tox_ask_toxicity,
                               tox_suppress_threshold_);
    }
    if (supp.queue_bid) {
        bpt::common::log::info(kLog(),
                               "{} queue suppress bids: fp={:.5f} < {:.5f} at px={:.4f}",
                               st.symbol,
                               supp.fp_bid,
                               queue_suppress_fill_prob_min_,
                               new_bid);
    }
    if (supp.queue_ask) {
        bpt::common::log::info(kLog(),
                               "{} queue suppress asks: fp={:.5f} < {:.5f} at px={:.4f}",
                               st.symbol,
                               supp.fp_ask,
                               queue_suppress_fill_prob_min_,
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
    if (st.bid_order_id != 0 && !st.bid_cancel_pending) {
        // Adverse selection guard: cancel if mid has risen significantly since
        // we placed this bid — informed flow is pushing against us.
        const bool adverse =
            st.bid_placed_mid > 0.0 && (mid - st.bid_placed_mid) / st.bid_placed_mid > requote_threshold_;
        // Model drift: modify-in-place if the AS model wants a different price.
        const bool stale =
            st.last_bid_price > 0.0 && std::abs(new_bid - st.last_bid_price) / st.last_bid_price > requote_threshold_;
        // Must cancel if at max inventory, adverse selection, or drift suppression.
        if (at_max_long || adverse || final_suppress_bids) {
            // Hard cancel — don't amend.
            //
            // Set bid_cancel_pending BEFORE the call so the in-process
            // backtest path — where cancel_order() runs the CANCELLED
            // ExecReport synchronously inside the call before returning
            // — sees the flag as `true` when on_exec_report tries to
            // clear it. Otherwise: sync handler clears the flag (which
            // wasn't set yet), call returns, caller sets flag to true,
            // and nothing ever clears it again → strategy goes silent
            // forever after the first cancel. The Aeron path is
            // unaffected (still async, still gets cleared by the next
            // poll-loop iteration's on_exec_report).
            st.bid_cancel_pending = true;
            if (order_mgr_) {
                bpt::common::log::debug(kLog(),
                                        "Cancel bid order_id={} {} @ {} reason={}",
                                        st.bid_order_id,
                                        st.symbol,
                                        st.exchange,
                                        at_max_long           ? "max_inv"
                                        : final_suppress_bids ? "suppress"
                                                              : "adverse");
                order_mgr_->send_cancel(order::CancelOrderRequest{st.bid_order_id, st.exchange_id, instrument_id});
            }
        } else if (stale) {
            // Price drift — amend in place to preserve queue position.
            if (order_mgr_) {
                double price = new_bid;
                if (st.tick_size > 0.0)
                    price = std::floor(price / st.tick_size) * st.tick_size;
                const int64_t price_fixed = static_cast<int64_t>(std::round(price * kPriceScale));
                const uint64_t qty_fp = static_cast<uint64_t>(std::round(eff_qty * 1e8));
                bpt::common::log::debug(kLog(),
                                        "Modify bid order_id={} {} @ {} → {:.6f}",
                                        st.bid_order_id,
                                        st.symbol,
                                        st.exchange,
                                        price);
                order_mgr_->modify_order(st.bid_order_id, st.exchange_id, instrument_id, price_fixed, qty_fp);
            }
            st.last_bid_price = new_bid;
            st.bid_placed_mid = mid;
        }
    }

    if (st.bid_order_id == 0 && !st.bid_cancel_pending && !at_max_long && !final_suppress_bids) {
        const uint64_t oid = send_limit_order(instrument_id, st, bpt::messages::OrderSide::BUY, new_bid, eff_qty);
        if (oid != 0) {
            st.bid_order_id = oid;
            st.last_bid_price = new_bid;
            st.bid_placed_mid = mid;
        }
    }

    // ── Ask side ──────────────────────────────────────────────────────────
    if (st.ask_order_id != 0 && !st.ask_cancel_pending) {
        // Adverse selection guard: cancel if mid has dropped since we placed this ask.
        const bool adverse =
            st.ask_placed_mid > 0.0 && (st.ask_placed_mid - mid) / st.ask_placed_mid > requote_threshold_;
        const bool stale =
            st.last_ask_price > 0.0 && std::abs(new_ask - st.last_ask_price) / st.last_ask_price > requote_threshold_;
        if (at_max_short || adverse || final_suppress_asks) {
            // Set before the call — see bid-side comment above.
            st.ask_cancel_pending = true;
            if (order_mgr_) {
                bpt::common::log::debug(kLog(),
                                        "Cancel ask order_id={} {} @ {} reason={}",
                                        st.ask_order_id,
                                        st.symbol,
                                        st.exchange,
                                        at_max_short          ? "max_inv"
                                        : final_suppress_asks ? "suppress"
                                                              : "adverse");
                order_mgr_->send_cancel(order::CancelOrderRequest{st.ask_order_id, st.exchange_id, instrument_id});
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
                                        st.ask_order_id,
                                        st.symbol,
                                        st.exchange,
                                        price);
                order_mgr_->modify_order(st.ask_order_id, st.exchange_id, instrument_id, price_fixed, qty_fp);
            }
            st.last_ask_price = new_ask;
            st.ask_placed_mid = mid;
        }
    }

    if (st.ask_order_id == 0 && !st.ask_cancel_pending && !at_max_short && !final_suppress_asks) {
        const uint64_t oid = send_limit_order(instrument_id, st, bpt::messages::OrderSide::SELL, new_ask, eff_qty);
        if (oid != 0) {
            st.ask_order_id = oid;
            st.last_ask_price = new_ask;
            st.ask_placed_mid = mid;
        }
    }

    // ── Active inventory unwind ────────────────────────────────────────────
    // When inventory exceeds max_inventory_, send an aggressive LIMIT IOC order
    // to reduce it rather than waiting passively for resting orders to fill.
    if (st.unwind_order_id == 0) {
        if (at_max_long) {
            const uint64_t oid = send_unwind_order(instrument_id, st, bpt::messages::OrderSide::SELL, mid, eff_qty);
            if (oid != 0)
                st.unwind_order_id = oid;
        } else if (at_max_short) {
            const uint64_t oid = send_unwind_order(instrument_id, st, bpt::messages::OrderSide::BUY, mid, eff_qty);
            if (oid != 0)
                st.unwind_order_id = oid;
        }
    }
}

uint64_t AvellanedaStoikovStrategy::send_limit_order(uint64_t instrument_id,
                                                     InstrumentState& st,
                                                     bpt::messages::OrderSide::Value side,
                                                     double price,
                                                     double qty) {
    const auto vex_it = venue_exec_.find(st.exchange);
    if (vex_it == venue_exec_.end() || !vex_it->second.enabled) {
        bpt::common::log::debug(kLog(), "Venue {} not enabled — quote suppressed", st.exchange);
        return 0;
    }

    if (!order_mgr_) {
        bpt::common::log::info(kLog(),
                               "{} {} {} @ {:.6f} (no gateway)",
                               (side == OrderSide::BUY ? "BID" : "ASK"),
                               st.symbol,
                               st.exchange,
                               price);
        return 0;
    }

    // Note: OrderManager rounds BUY up and SELL down. For market-making, we want
    // the opposite (bid floors, ask ceils) to preserve spread width, so pre-round here.
    if (st.tick_size > 0.0) {
        if (side == OrderSide::BUY)
            price = std::floor(price / st.tick_size) * st.tick_size;
        else
            price = std::ceil(price / st.tick_size) * st.tick_size;
    }

    const uint64_t order_id = order_mgr_->send_new_order(order::NewOrderRequest{
        .instrument_id = instrument_id,
        .exchange_id = st.exchange_id,
        .side = side,
        .type = OrderType::LIMIT,
        .tif = TimeInForce::GTC,
        .price = price,
        .qty = qty,
        .exec_inst = {.post_only = true},
    });
    if (order_id == 0)
        return 0;

    bpt::common::log::info(kLog(),
                           "{} {} {} @ {:.6f} → order_id={}",
                           (side == OrderSide::BUY ? "BID" : "ASK"),
                           st.symbol,
                           st.exchange,
                           price,
                           order_id);

    order_to_instrument_[order_id] = instrument_id;
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
    return order_id;
}

uint64_t AvellanedaStoikovStrategy::send_unwind_order(uint64_t instrument_id,
                                                      InstrumentState& st,
                                                      bpt::messages::OrderSide::Value side,
                                                      double mid,
                                                      double qty) {
    const auto vex_it = venue_exec_.find(st.exchange);
    if (vex_it == venue_exec_.end() || !vex_it->second.enabled)
        return 0;

    if (!order_mgr_) {
        bpt::common::log::info(kLog(),
                               "UNWIND {} {} @ {} mid={:.6f} (no gateway)",
                               (side == OrderSide::BUY ? "BUY" : "SELL"),
                               st.symbol,
                               st.exchange,
                               mid);
        return 0;
    }

    // Cross the spread aggressively — shutdown_cross_bps through mid — to
    // ensure immediate fill. Default 20 bps fits major pairs in normal
    // regimes; raise for thin venues or volatile shutdowns (see the
    // shutdown_cross_bps knob in [strategy.params]).
    // Use LIMIT IOC rather than MARKET to avoid OKX SPOT market-buy qty quirks
    // (OKX interprets SPOT market BUY sz as quote currency, not base).
    const double cross_factor = 1.0 + (shutdown_cross_bps_ / 10000.0);
    const double price = (side == OrderSide::BUY) ? mid * cross_factor : mid / cross_factor;

    const uint64_t order_id = order_mgr_->send_new_order(order::NewOrderRequest{
        .instrument_id = instrument_id,
        .exchange_id = st.exchange_id,
        .side = side,
        .type = OrderType::LIMIT,
        .tif = TimeInForce::IOC,
        .price = price,
        .qty = qty,
    });
    if (order_id == 0)
        return 0;

    bpt::common::log::info(kLog(),
                           "UNWIND {} {} @ {} price={:.6f} mid={:.6f} → order_id={}",
                           (side == OrderSide::BUY ? "BUY" : "SELL"),
                           st.symbol,
                           st.exchange,
                           price,
                           mid,
                           order_id);

    order_to_instrument_[order_id] = instrument_id;
    return order_id;
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
            if (st.bid_order_id != 0 && !st.bid_cancel_pending) {
                order_mgr_->send_cancel(order::CancelOrderRequest{st.bid_order_id, st.exchange_id, inst_id});
                st.bid_cancel_pending = true;
            }
            if (st.ask_order_id != 0 && !st.ask_cancel_pending) {
                order_mgr_->send_cancel(order::CancelOrderRequest{st.ask_order_id, st.exchange_id, inst_id});
                st.ask_cancel_pending = true;
            }
        }
    } else {
        bpt::common::log::info(kLog(), "Refdata heartbeat resumed — quoting re-enabled");
    }
}

// get_strategy_state_json / save_state / load_state → avellaneda_stoikov_state_io.cpp
// on_shutdown_flatten / on_account_snapshot / has_pending_flatten → avellaneda_stoikov_shutdown.cpp

}  // namespace bpt::strategy::strategy
