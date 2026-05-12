#include "strategy/strategy/avellaneda_stoikov_strategy.h"

#include "strategy/clock/sim_clock.h"
#include "strategy/strategy/reconciler.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/exec_inst.h>
#include <messages/InstrumentType.h>
#include <messages/OrderType.h>
#include <messages/RejectSource.h>
#include <messages/TimeInForce.h>
#include <messages/TradeSide.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

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
    if (mode == "mid")                c.mode = FairValueEstimator::Mode::kMid;
    else if (mode == "micro")         c.mode = FairValueEstimator::Mode::kMicro;
    else if (mode == "micro_capped")  c.mode = FairValueEstimator::Mode::kMicroSizeCapped;
    else if (mode == "l2_weighted")   c.mode = FairValueEstimator::Mode::kL2WeightedMicro;
    else if (mode == "ewma_micro")    c.mode = FairValueEstimator::Mode::kEwmaMicro;
    else
        bpt::common::log::warn(kLog(),
            "[fair_value] unknown mode='{}' — falling back to 'mid'", mode);
    c.size_cap_qty = fv["size_cap_qty"].value<double>().value_or(c.size_cap_qty);
    c.ladder_depth = static_cast<std::size_t>(
        fv["ladder_depth"].value<int64_t>().value_or(static_cast<int64_t>(c.ladder_depth)));
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
      drift_warmup_ticks_(static_cast<std::size_t>(
          cfg.params["drift_warmup_ticks"].value<int64_t>().value_or(50))),
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
      gamma_pnl_window_n_(static_cast<std::size_t>(
          cfg.params["gamma_pnl_window_n"].value<int64_t>().value_or(0))),
      gamma_pnl_loss_threshold_usd_(cfg.params["gamma_pnl_loss_threshold_usd"].value<double>().value_or(0.0)),
      gamma_pnl_profit_threshold_usd_(cfg.params["gamma_pnl_profit_threshold_usd"].value<double>().value_or(0.0)),
      gamma_pnl_widen_mult_(cfg.params["gamma_pnl_widen_mult"].value<double>().value_or(1.0)),
      gamma_pnl_tighten_mult_(cfg.params["gamma_pnl_tighten_mult"].value<double>().value_or(1.0)),
      ofi_weight_bps_(cfg.params["ofi_weight_bps"].value<double>().value_or(0.0)),
      ofi_window_ns_(static_cast<uint64_t>(
          cfg.params["ofi_window_ms"].value<double>().value_or(1000.0) * 1e6)),
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
            case FairValueEstimator::Mode::kMid:             fv_mode_str = "mid"; break;
            case FairValueEstimator::Mode::kMicro:           fv_mode_str = "micro"; break;
            case FairValueEstimator::Mode::kMicroSizeCapped: fv_mode_str = "micro_capped"; break;
            case FairValueEstimator::Mode::kL2WeightedMicro: fv_mode_str = "l2_weighted"; break;
            case FairValueEstimator::Mode::kEwmaMicro:       fv_mode_str = "ewma_micro"; break;
        }
        bpt::common::log::info(kLog(),
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

    std::vector<refdata::IRefdataClient::CanonicalFilter> filters;
    for (const auto& sym : instruments_) {
        if (auto parsed = CanonicalResolver::parse(sym)) {
            const auto sbe_type = [&]() {
                using T = refdata::InstrumentType;
                using S = bpt::messages::InstrumentType;
                switch (parsed->type) {
                    case T::SPOT:
                        return S::SPOT;
                    case T::PERPETUAL:
                        return S::PERPETUAL;
                    case T::FUTURE:
                        return S::FUTURE;
                    case T::OPTION:
                        return S::OPTION;
                    default:
                        return S::NULL_VALUE;
                }
            }();
            if (md_exchanges_.empty()) {
                filters.push_back({parsed->base, parsed->quote, sbe_type, ""});
            } else {
                for (const auto& ex : md_exchanges_)
                    filters.push_back({parsed->base, parsed->quote, sbe_type, ex});
            }
        }
    }
    refdata_.subscribe(correlation_id_, std::move(filters));
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

    const auto ids = CanonicalResolver::resolve(cache, instruments_, md_exchanges_);
    for (uint64_t id : ids) {
        const auto inst = cache.get(id);
        if (!inst)
            continue;

        auto ex_id = ExchangeId::NULL_VALUE;
        if (inst->exchange == "BINANCE")
            ex_id = ExchangeId::BINANCE;
        else if (inst->exchange == "OKX")
            ex_id = ExchangeId::OKX;
        else if (inst->exchange == "HYPERLIQUID")
            ex_id = ExchangeId::HYPERLIQUID;

        auto [it, inserted] = state_.emplace(id,
                       InstrumentState{.symbol = inst->symbol,
                                       .exchange = inst->exchange,
                                       .exchange_id = ex_id,
                                       .instrument_type = inst->type,
                                       .base_ccy = inst->base_currency,
                                       .tick_size = inst->tick_size,
                                       .lot_size = inst->lot_size,
                                       .vol_gate = VolatilityGate(vol_gate_cfg_),
                                       .regime = RegimeDetector(regime_cfg_)});
        if (inserted) {
            it->second.fv = FairValueEstimator{fv_cfg_};
            it->second.ofi = OFICalculator{OFICalculator::Config{
                .max_levels = static_cast<int>(order_book_depth_ > 0 ? order_book_depth_ : 5),
                .window_ns  = ofi_window_ns_,
            }};
        }
        bpt::common::log::info("  [{}] {} @ {} tick={} lot={}",
                               id,
                               inst->symbol,
                               inst->exchange,
                               inst->tick_size,
                               inst->lot_size);
    }

    bpt::common::log::info(kLog(), "Trading universe: {} instrument(s)", state_.size());

    if (!md_client_)
        return;

    std::vector<md::IMdClient::InstrumentDesc> subs;
    subs.reserve(state_.size());
    for (const auto& [id, st] : state_)
        subs.push_back({id, st.exchange, st.symbol, order_book_depth_});

    bpt::common::log::info(kLog(),
                           "Subscribing MD to {} instrument(s) depth={}",
                           subs.size(),
                           static_cast<int>(order_book_depth_));
    md_client_->subscribe(correlation_id_, subs);
}

void AvellanedaStoikovStrategy::on_delta(const refdata::Instrument& inst,
                                         bpt::messages::DeltaUpdateType::Value update_type) {
    if (update_type == bpt::messages::DeltaUpdateType::ADD) {
        const auto ids = CanonicalResolver::resolve(refdata_.cache(), instruments_, md_exchanges_);
        if (std::find(ids.begin(), ids.end(), inst.instrument_id) == ids.end())
            return;

        using EX = bpt::messages::ExchangeId;
        auto ex_id = EX::NULL_VALUE;
        if (inst.exchange == "BINANCE")
            ex_id = EX::BINANCE;
        else if (inst.exchange == "OKX")
            ex_id = EX::OKX;
        else if (inst.exchange == "HYPERLIQUID")
            ex_id = EX::HYPERLIQUID;

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
                .window_ns  = ofi_window_ns_,
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
    if (ofi_weight_bps_ != 0.0 && st.book.ready()) {
        const std::size_t K = order_book_depth_ > 0 ? order_book_depth_ : 5;
        const auto top_b = st.book.top_bids(K);
        const auto top_a = st.book.top_asks(K);
        std::vector<OFICalculator::Level> ofi_bids, ofi_asks;
        ofi_bids.reserve(top_b.size());
        ofi_asks.reserve(top_a.size());
        for (const auto& l : top_b) ofi_bids.emplace_back(l.price, l.qty);
        for (const auto& l : top_a) ofi_asks.emplace_back(l.price, l.qty);
        st.ofi.update(ofi_bids, ofi_asks, book.timestampNs());
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

    // Cache market top-of-book for the dashboard overlay. Done here (not
    // just before publish) so the strategy sees the same values the
    // dashboard does, and works even when order_book_depth=0 leaves
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
        const double markout_bps =
            (mid - st.pending_buy_fill_price) / st.pending_buy_fill_price * 1e4;
        if (markout_bps < post_fill_markout_threshold_bps_) {
            const uint64_t cooldown_ns =
                static_cast<uint64_t>(post_fill_markout_cooldown_s_ * 1e9);
            st.post_fill_suspend_until_bid = ts_ns + cooldown_ns;
            bpt::common::log::warn(
                kLog(),
                "{} post-fill BUY markout {:.2f} bps < {:.2f} — suspending bid for {:.1f}s",
                st.symbol, markout_bps,
                post_fill_markout_threshold_bps_, post_fill_markout_cooldown_s_);
        }
        st.pending_buy_fill_price = 0.0;
    }
    if (st.pending_sell_fill_price > 0.0) {
        // SELL favorable = mid moved DOWN, so flip sign vs BUY case.
        const double markout_bps =
            (st.pending_sell_fill_price - mid) / st.pending_sell_fill_price * 1e4;
        if (markout_bps < post_fill_markout_threshold_bps_) {
            const uint64_t cooldown_ns =
                static_cast<uint64_t>(post_fill_markout_cooldown_s_ * 1e9);
            st.post_fill_suspend_until_ask = ts_ns + cooldown_ns;
            bpt::common::log::warn(
                kLog(),
                "{} post-fill SELL markout {:.2f} bps < {:.2f} — suspending ask for {:.1f}s",
                st.symbol, markout_bps,
                post_fill_markout_threshold_bps_, post_fill_markout_cooldown_s_);
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
        st.slow_drift_bps =
            (mid - st.slow_drift_window_start_mid) / st.slow_drift_window_start_mid * 1e4;
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
        st.vol_gate.set_max_bps_per_window(
            std::max(vol_gate_cfg_.max_bps_per_window, adaptive_bps));
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
                order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, tick.instrumentId());
                st.bid_cancel_pending = true;
            }
            if (st.ask_order_id != 0 && !st.ask_cancel_pending) {
                order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, tick.instrumentId());
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
                st.pending_buy_fill_ts    = st.last_tick_ns;
            } else {
                st.pending_sell_fill_price = fill_px;
                st.pending_sell_fill_ts    = st.last_tick_ns;
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
            if (pause_below_rpnl_usd_ < 0.0 &&
                pos->realized_pnl < pause_below_rpnl_usd_ &&
                prior_rpnl >= pause_below_rpnl_usd_) {
                const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
                st.pause_until_ns = now_ns + static_cast<uint64_t>(pause_cooldown_s_ * 1e9);
                bpt::common::log::warn(kLog(),
                    "{} PAUSE TRIGGERED rpnl={:.4f} crossed below threshold={:.4f} — "
                    "halting both sides for {:.0f}s",
                    st.symbol, pos->realized_pnl, pause_below_rpnl_usd_, pause_cooldown_s_);
                if (order_mgr_) {
                    if (st.bid_order_id != 0 && !st.bid_cancel_pending) {
                        order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, canonical_id);
                        st.bid_cancel_pending = true;
                    }
                    if (st.ask_order_id != 0 && !st.ask_cancel_pending) {
                        order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, canonical_id);
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

bool AvellanedaStoikovStrategy::compute_quotes(const InstrumentState& st,
                                               uint64_t instrument_id,
                                               double net_qty,
                                               double mid,
                                               uint64_t timestamp_ns,
                                               double& out_bid,
                                               double& out_ask) const {
    if (st.ewma_ticks < vol_warmup_ticks_)
        return false;
    if (st.ewma_var <= 0.0)
        return false;

    // Remaining session time — clamp to [0, session_duration_s_].
    // After the session ends we keep quoting at the minimum spread (T-t = 0).
    const double elapsed_s = static_cast<double>(timestamp_ns - st.session_start_ns) * 1e-9;
    const double T_minus_t = std::max(0.0, session_duration_s_ - elapsed_s);

    const double sigma_sq = st.ewma_var;
    // Regime-adjusted gamma: widen spreads in trending regimes, tighten
    // in mean-reverting regimes. The multiplier comes from the Hurst-based
    // regime detector (1.8x in trending, 0.6x in mean-reverting, 1.0x neutral).
    // Effective γ folds in two adaptive factors:
    //   1. Regime detector multiplier (mean-rev / neutral / trending)
    //   2. PnL feedback multiplier (widen on recent loss streak)
    // Both default to 1.0 when their respective features are disabled,
    // so static γ behavior is unchanged unless operator opts in.
    const double effective_gamma =
        gamma_ * st.regime.gamma_multiplier() * gamma_pnl_mult(st);
    const double gamma_sigma_sq_T = effective_gamma * sigma_sq * T_minus_t;

    // Drift-adjusted reservation price (Cartea-Jaimungal extension).
    // Classic AS: r = s - q*γ*σ²*(T-t)               (assumes µ=0)
    // With drift: r = s + µ*(T-t) - q*γ*σ²*(T-t)     (leans into the trend)
    //
    // Implementation note — dimensional handling: σ² and µ here are
    // computed from log-returns (dimensionless), not from price changes
    // ($²). The textbook formula treats q*γ*σ²*T as a price-units
    // displacement; with log-return σ² that product is a fraction.
    // Multiplying by `mid` converts it back to price units. q is also
    // normalized to [-1, 1] via max_inventory_ so γ is scale-invariant
    // across instruments — same γ=0.05 produces ~3% max skew on APE
    // ($0.16) and on BTC ($30k). Without the normalization the formula
    // silently broke on cheap instruments (e.g. APE: q=100 produced a
    // $2.74 inventory penalty against a $0.16 mid, blowing reservation
    // negative — see commit b684b17 for the empirical trace).
    //
    // When µ > 0 (uptrend), reservation rises above mid → asks move up
    // (harder to get filled short), bids move up (easier to get filled long).
    // This counteracts the core AS weakness of accumulating adverse inventory
    // in momentum regimes.
    const double q_normalized = (max_inventory_ > 0.0)
                                    ? std::clamp(net_qty / max_inventory_, -1.0, 1.0)
                                    : 0.0;
    const double inventory_skew_frac = q_normalized * gamma_sigma_sq_T;
    // Drift contribution to reservation. ewma_drift is the EWMA of
    // log_ret/√dt — units of log-returns per √second. Integrating that
    // over a horizon T gives a dimensionless cumulative drift of
    // µ·√T (Itô convention for a Brownian µ·dt term with µ measured per
    // √s). The pre-fix code used µ·T which has units log_ret·√s, off
    // by a factor of √T from the correct dimensionless form. On HL APE
    // with µ ≈ -7e-4 per √s and T = 3600 s, that error sent
    // drift_skew_frac to -2.52 (-252% of mid) routinely; the b684b17
    // sanity clamp absorbed the spikes but the distribution was still
    // skewed. Switching to √T brings drift_skew_frac to ~-0.042 (-4.2%)
    // for the same inputs — bounded and dimensionally honest.
    // Suppress drift skew during warmup — early ewma_drift values are
    // noisy enough to push reservation through the touch. After
    // drift_warmup_ticks_ BBO updates, the EWMA has settled enough that
    // its sqrt(T - t) projection is a meaningful directional bias.
    double drift_skew_frac = (st.drift_ticks >= drift_warmup_ticks_)
                                 ? st.ewma_drift * std::sqrt(T_minus_t)
                                 : 0.0;
    // Hard cap on drift skew magnitude. Without it, strong intraday
    // trends amplified by √(T-t) at session start drive reservation
    // 50+ bps from mid, putting quotes deeper than any realistic book
    // cross. The drift signal is still in play (suppression checks on
    // ewma_drift remain unchanged); cap only bounds how far reservation
    // can be moved by drift alone.
    if (max_drift_skew_bps_ > 0.0) {
        const double cap = max_drift_skew_bps_ / 10000.0;
        drift_skew_frac = std::clamp(drift_skew_frac, -cap, cap);
    }
    // OFI skew (Cont-Kukanov-Stoikov) — additive contribution to the
    // reservation proportional to the rolling normalized OFI signal.
    // Sign matches drift_skew_frac: positive OFI = buy pressure, lifts
    // reservation above mid → asks move up (harder to fill short),
    // bids move up (easier to fill long), opposite of how AS handles
    // accumulating inventory. ofi_weight_bps_ = 0 (default) → no-op.
    const double ofi_skew_frac = ofi_weight_bps_ * 1e-4 * st.ofi.value();
    const double reservation = mid * (1.0 + drift_skew_frac + ofi_skew_frac - inventory_skew_frac);
    // Kept for the debug log below; same as drift_skew_frac * mid.
    const double drift_adjustment = drift_skew_frac * mid;

    // Minimum half-spread: config floor + round-trip maker fee so we never
    // quote a spread that is guaranteed to lose money to commissions.
    // fee_half = maker_bps / 10000 * mid (one leg); both legs = 2x, so each
    // side of the spread must cover at least 1x maker fee.
    double fee_half_spread = 0.0;
    const auto fee_entry = refdata_.fee_cache().get(st.exchange_id, instrument_id, timestamp_ns);
    if (fee_entry) {
        fee_half_spread = (static_cast<double>(fee_entry->maker_bps) / 10000.0) * mid;
    }

    // Use live EWMA κ once warmed up; fall back to config kappa_ before then.
    // Floor at kappa_min_ to prevent ln(1 + γ/κ) → ∞ as κ → 0.
    const double kappa = (st.kappa_ticks >= kappa_warmup_ticks_) ? std::max(kappa_min_, st.ewma_kappa) : kappa_;

    const double min_half_spread = std::max((min_half_spread_bps_ / 10000.0) * mid, fee_half_spread);
    const double raw_half_spread =
        std::max(min_half_spread,
                 gamma_sigma_sq_T / 2.0 + (1.0 / effective_gamma) * std::log(1.0 + effective_gamma / kappa));

    // Cold-start / pathological-σ² clamp. The AS formula can produce
    // absurdly wide half-spreads before warmup settles or if σ² or κ
    // estimates go haywire. max_half_spread_bps_ is the "never quote
    // wider than this" sanity ceiling. If we hit it, warmup isn't done
    // or something in the EWMA updater is off — log at WARN, rate-limited,
    // so ops see it but logs don't flood.
    const double max_half_spread = (max_half_spread_bps_ / 10000.0) * mid;
    double half_spread = raw_half_spread;
    if (raw_half_spread > max_half_spread) {
        half_spread = max_half_spread;
        static std::size_t clamp_count = 0;
        if (++clamp_count <= 5 || clamp_count % 1000 == 0) {
            bpt::common::log::warn(
                kLog(),
                "half-spread clamp: formula={:.2f} bps → clamped to {:.2f} bps "
                "(σ²={:.2e} κ={:.4f} ticks={} {}; {} clamps so far)",
                raw_half_spread / mid * 10000,
                max_half_spread_bps_,
                sigma_sq,
                kappa,
                st.ewma_ticks,
                (st.ewma_ticks < vol_warmup_ticks_ * 3) ? "WARMUP" : "σ-SPIKE",
                clamp_count);
        }
    }

    out_bid = reservation - half_spread;
    out_ask = reservation + half_spread;

    // ── Reservation-skew cap ────────────────────────────────────────────
    //
    // Inventory pressure can push the reservation through the touch
    // (when net_qty * γ * σ² * T > spread/2). Without a cap, AS posts
    // BIDs at or above the best ask, or ASKs at or below the best bid —
    // POST_ONLY orders that the venue rejects, GTC orders that pay
    // taker fees. Either way: not the maker behaviour AS is designed
    // for.
    //
    // Clamp each side to strictly inside the BBO by one tick. Skipped
    // when the cached BBO isn't valid (cold start, gap, etc.) — better
    // to let the unclamped quote through than block on missing data.
    //
    // Effect: AS still skews aggressively toward the inventory-unwind
    // side (e.g. when long, the ASK tightens), but neither side is
    // allowed to cross. Real exchanges treat at-touch quotes as
    // contestable maker fills, so the −tick clamp is conservative;
    // tightening to −0 (touch) would be an option later.
    if (st.tick_size > 0.0 && st.last_market_bid > 0.0 && st.last_market_ask > 0.0) {
        const double bid_cap = st.last_market_ask - st.tick_size;
        const double ask_floor = st.last_market_bid + st.tick_size;
        if (out_bid > bid_cap) out_bid = bid_cap;
        if (out_ask < ask_floor) out_ask = ask_floor;
        // Defensive: if the clamp inverts the spread (only possible on
        // a crossed market, which shouldn't happen but might in
        // transient feed states), treat as "don't quote this tick."
        if (out_bid >= out_ask)
            return false;
    }

    // ── Final sanity check on quote level ───────────────────────────────
    //
    // Even after every cap above, the formula can land on absurd quote
    // levels — most commonly on cheap instruments where the inventory
    // penalty (q*γ*σ²*T) is dimensionally wrong and overwhelms mid. APE
    // at $0.16 produced bids at -$2.74 in the 2026-05-07 backtest; the
    // OrderManager rejected 899 of them in 11h because price ≤ 0. By the
    // time OrderMgr saw them, the strategy had already built and tracked
    // an order. Cheaper to skip the whole tick here.
    //
    // Bound is symmetric around mid in bps. Fires also on cold start
    // (last_market_bid/ask ≈ 0 makes the post-touch cap silently no-op).
    if (st.last_mid > 0.0 && quote_sanity_bps_ > 0.0) {
        const double bound = st.last_mid * (quote_sanity_bps_ / 10000.0);
        const double lo = st.last_mid - bound;
        const double hi = st.last_mid + bound;
        if (out_bid < lo || out_ask > hi || out_bid <= 0.0) {
            static std::size_t skip_count = 0;
            if (++skip_count <= 5 || skip_count % 1000 == 0) {
                bpt::common::log::warn(
                    kLog(),
                    "{} quote out of sanity range — skipping tick: "
                    "bid={:.6f} ask={:.6f} mid={:.6f} reservation={:.6f} "
                    "half_spread={:.6f} (sanity_bps={:.1f}; {} skips so far)",
                    st.symbol,
                    out_bid,
                    out_ask,
                    st.last_mid,
                    reservation,
                    half_spread,
                    quote_sanity_bps_,
                    skip_count);
            }
            return false;
        }
    }

    bpt::common::log::debug(
        kLog(),
        "quotes σ²={:.2e} µ={:.2e} κ={:.4f} ({}) half_spread={:.4f} reservation={:.2f} drift_adj={:.4f}",
        sigma_sq,
        st.ewma_drift,
        kappa,
        (st.kappa_ticks >= kappa_warmup_ticks_) ? "live" : "fallback",
        half_spread,
        reservation,
        drift_adjustment);

    return true;
}

// ── Effective sizing — adaptive vs fixed ───────────────────────────────────
double AvellanedaStoikovStrategy::effective_order_qty(const InstrumentState& st) const {
    if (order_qty_fraction_ > 0.0 && last_equity_e8_ > 0 && st.last_mid > 0.0) {
        const double equity_usd = static_cast<double>(last_equity_e8_) / 1e8;
        const double derived = order_qty_fraction_ * equity_usd / st.last_mid;
        return std::max(order_qty_min_, derived);
    }
    return order_qty_;
}

double AvellanedaStoikovStrategy::effective_max_inventory(const InstrumentState& st) const {
    if (max_inventory_fraction_ > 0.0 && last_equity_e8_ > 0 && st.last_mid > 0.0) {
        const double equity_usd = static_cast<double>(last_equity_e8_) / 1e8;
        return max_inventory_fraction_ * equity_usd / st.last_mid;
    }
    return max_inventory_;
}

double AvellanedaStoikovStrategy::gamma_pnl_mult(const InstrumentState& st) const {
    // Disabled if window not configured. Also a no-op until at least
    // one fill has accrued — empty deque sums to 0, which falls into
    // the deadband by design (no over-eager widen on session start).
    if (gamma_pnl_window_n_ == 0 || st.recent_rpnl.empty())
        return 1.0;
    double sum = 0.0;
    for (double r : st.recent_rpnl) sum += r;
    if (sum < gamma_pnl_loss_threshold_usd_)   return gamma_pnl_widen_mult_;
    if (sum > gamma_pnl_profit_threshold_usd_) return gamma_pnl_tighten_mult_;
    return 1.0;
}

// ── Suppression state — single source of truth for both runtime
//    (maybe_requote) and dashboard (get_strategy_state_json) ────────────────
AvellanedaStoikovStrategy::SuppressionState
AvellanedaStoikovStrategy::compute_suppression(const InstrumentState& st,
                                               double net_qty,
                                               double new_bid,
                                               double new_ask) const {
    SuppressionState s;

    // Inventory cap — hardest blocker (we never want to add beyond max).
    const double max_inv = effective_max_inventory(st);
    s.inventory_bid = net_qty >= max_inv;
    s.inventory_ask = net_qty <= -max_inv;

    // Phase 2.1 — per-side post-fill cooldown after an adverse fill.
    // Driven by the markout evaluation in on_bbo (writes
    // post_fill_suspend_until_*); 0 means no cooldown active. Compares
    // against st.last_tick_ns (simulation time) so backtest replays —
    // which compress 11h of sim time into a few seconds of wall clock —
    // honor the cooldown window correctly.
    if (post_fill_markout_threshold_bps_ < 0.0) {
        s.post_fill_bid = st.post_fill_suspend_until_bid > 0
                       && st.last_tick_ns < st.post_fill_suspend_until_bid;
        s.post_fill_ask = st.post_fill_suspend_until_ask > 0
                       && st.last_tick_ns < st.post_fill_suspend_until_ask;
    }

    // Intra-tick realized-vol gate — blocks BOTH sides during fast moves.
    s.vol_halted = st.vol_gate.is_halted(st.last_tick_ns);

    // Drawdown circuit-breaker — blocks BOTH sides while pause window
    // is active. Set in on_exec_report when realized PnL crosses the
    // configured loss threshold; resumes implicitly via the timestamp
    // check (no explicit resume event).
    s.pause_active = st.pause_until_ns > 0 && st.last_tick_ns < st.pause_until_ns;

    // Current σ in bps/√s — derived from the per-√s² variance EWMA
    // AS already maintains. Used to scale drift, slow-drift, and
    // vol-gate thresholds adaptively so one set of k-multiples works
    // across assets and vol regimes (see drift_suppress_sigma_mult_
    // docstring). 0 when EWMA hasn't warmed; adaptive part is then
    // a no-op (threshold stays at the fixed floor).
    const double sigma_bps = st.ewma_var > 0.0 ? std::sqrt(st.ewma_var) * 1e4 : 0.0;

    // Drift (fast): suppress the adverse side when |µ| > threshold.
    // Threshold = max(fixed_floor, sigma_mult × σ_bps). With sigma_mult
    // = 3, this fires on ~3-SD-per-√s moves regardless of asset.
    const double drift_bps = std::abs(st.ewma_drift) * 1e4;
    const double drift_threshold_bps =
        std::max(drift_suppress_bps_, drift_suppress_sigma_mult_ * sigma_bps);
    const bool drift_on = drift_threshold_bps > 0.0 && drift_bps > drift_threshold_bps;
    s.drift_ask = drift_on && st.ewma_drift > 0.0;  // uptrend → don't sell
    s.drift_bid = drift_on && st.ewma_drift < 0.0;  // downtrend → don't buy

    // Trend (slow): keyed on cumulative return over slow_drift_window_s_.
    // Expected stdev of a window-cumulative return ≈ σ × √window_s in
    // per-√s units, so the adaptive threshold multiplies σ_bps by the
    // window time-scale √window_s. Setting sigma_mult = 3 ≈ "3-SD
    // cumulative move over the window."
    const double trend_bps = std::abs(st.slow_drift_bps);
    const double trend_threshold_bps = std::max(
        slow_drift_suppress_bps_,
        slow_drift_suppress_sigma_mult_ * sigma_bps * std::sqrt(slow_drift_window_s_));
    const bool trend_on = trend_threshold_bps > 0.0 && trend_bps > trend_threshold_bps;
    s.trend_ask = trend_on && st.slow_drift_bps > 0.0;
    s.trend_bid = trend_on && st.slow_drift_bps < 0.0;

    // Toxicity: suppress a side when analytics reports its 5s markout
    // below threshold. Outcome-based (realized markout from our own
    // fills), complements drift's signal-based approach. Threshold is
    // negative; 0 disables.
    if (tox_suppress_threshold_ < 0.0 && st.tox_data_received) {
        s.tox_bid = st.tox_bid_toxicity < tox_suppress_threshold_;
        s.tox_ask = st.tox_ask_toxicity < tox_suppress_threshold_;
    }

    // Queue position: project fill_prob at the candidate quote price
    // using the live ladder. Below queue_suppress_fill_prob_min_, we'd
    // sit buried behind enough size that expected fill accumulates
    // stale-inventory risk without meaningful edge. Default fp = 1.0
    // when the book isn't ready — i.e. don't suppress, quoting wins.
    if (queue_suppress_fill_prob_min_ > 0.0 && st.book.ready()) {
        const double qa_bid = st.book.bid_vol_above(new_bid) + st.book.size_at_bid(new_bid);
        const double qa_ask = st.book.ask_vol_below(new_ask) + st.book.size_at_ask(new_ask);
        const double qty = effective_order_qty(st);
        s.fp_bid = qty / (qty + qa_bid);
        s.fp_ask = qty / (qty + qa_ask);
        s.queue_bid = s.fp_bid < queue_suppress_fill_prob_min_;
        s.queue_ask = s.fp_ask < queue_suppress_fill_prob_min_;
    }

    return s;
}

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

    // Info-level logging of the runtime triggers. Dashboard reporting
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
                               st.symbol, st.tox_bid_toxicity, tox_suppress_threshold_);
    }
    if (supp.tox_ask) {
        bpt::common::log::info(kLog(),
                               "{} tox suppress asks: score={:.2f} < {:.2f}",
                               st.symbol, st.tox_ask_toxicity, tox_suppress_threshold_);
    }
    if (supp.queue_bid) {
        bpt::common::log::info(kLog(),
                               "{} queue suppress bids: fp={:.5f} < {:.5f} at px={:.4f}",
                               st.symbol, supp.fp_bid, queue_suppress_fill_prob_min_, new_bid);
    }
    if (supp.queue_ask) {
        bpt::common::log::info(kLog(),
                               "{} queue suppress asks: fp={:.5f} < {:.5f} at px={:.4f}",
                               st.symbol, supp.fp_ask, queue_suppress_fill_prob_min_, new_ask);
    }

    // Legacy variable names retained for the side-decision blocks below
    // — match existing log-message key naming (`max_inv` vs `suppress`)
    // so the operational log format is unchanged by the refactor.
    const bool at_max_long  = supp.inventory_bid;
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
            if (order_mgr_) {
                bpt::common::log::debug(kLog(),
                                        "Cancel bid order_id={} {} @ {} reason={}",
                                        st.bid_order_id,
                                        st.symbol,
                                        st.exchange,
                                        at_max_long           ? "max_inv"
                                        : final_suppress_bids ? "suppress"
                                                              : "adverse");
                order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, instrument_id);
            }
            st.bid_cancel_pending = true;
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
            if (order_mgr_) {
                bpt::common::log::debug(kLog(),
                                        "Cancel ask order_id={} {} @ {} reason={}",
                                        st.ask_order_id,
                                        st.symbol,
                                        st.exchange,
                                        at_max_short          ? "max_inv"
                                        : final_suppress_asks ? "suppress"
                                                              : "adverse");
                order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, instrument_id);
            }
            st.ask_cancel_pending = true;
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

    const uint64_t order_id =
        order_mgr_
            ->place_order(instrument_id, st.exchange_id, side, OrderType::LIMIT, TimeInForce::GTC, price, qty,
                          bpt::messages::kExecInstPostOnly);
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

    const uint64_t order_id =
        order_mgr_->place_order(instrument_id, st.exchange_id, side, OrderType::LIMIT, TimeInForce::IOC, price, qty);
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
        bpt::common::log::warn(kLog(),
            "Refdata heartbeat stale — pausing new quotes, cancelling resting orders");
        // Cancel every live bid/ask. Without this, existing orders keep
        // filling at prices computed before the pause — which is exactly
        // the silent-bleed failure mode this gate is designed to prevent.
        // Same pattern as the vol_halted branch in on_bbo.
        if (!order_mgr_)
            return;
        for (auto& [inst_id, st] : state_) {
            if (st.bid_order_id != 0 && !st.bid_cancel_pending) {
                order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, inst_id);
                st.bid_cancel_pending = true;
            }
            if (st.ask_order_id != 0 && !st.ask_cancel_pending) {
                order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, inst_id);
                st.ask_cancel_pending = true;
            }
        }
    } else {
        bpt::common::log::info(kLog(),
            "Refdata heartbeat resumed — quoting re-enabled");
    }
}

// ── Strategy state for dashboard ────────────────────────────────────────────

std::string AvellanedaStoikovStrategy::get_strategy_state_json() {
    // Single instrument for now — take the first entry in state_.
    if (state_.empty())
        return {};

    const auto& [instrument_id, st] = *state_.begin();
    const double net_qty = static_cast<double>(positions_.net_qty(instrument_id, st.exchange_id)) / 1e8;

    // Compute current quotes to get reservation and half-spread.
    double bid_quote = 0, ask_quote = 0;
    bool quotes_valid = false;
    if (st.last_mid > 0 && st.ewma_ticks >= vol_warmup_ticks_) {
        quotes_valid = compute_quotes(st, instrument_id, net_qty, st.last_mid, st.last_tick_ns, bid_quote, ask_quote);
    }

    const double half_spread = quotes_valid ? (ask_quote - bid_quote) / 2.0 : 0.0;
    const double reservation = quotes_valid ? (bid_quote + ask_quote) / 2.0 : st.last_mid;
    const double reservation_offset_bps = st.last_mid > 0 ? (reservation - st.last_mid) / st.last_mid * 1e4 : 0.0;

    // Suppression snapshot shared with maybe_requote — single source of
    // truth so the dashboard badge can't disagree with the actual
    // runtime decision. Queue suppression is only meaningful when
    // quotes_valid (pre-warmup returns fp=1); compute_suppression
    // computes it unconditionally but the projected prices below are
    // still defaulted from the struct, which is correct since st.book
    // wouldn't be ready during warmup anyway.
    const SuppressionState supp = compute_suppression(st, net_qty, bid_quote, ask_quote);
    const double drift_bps = std::abs(st.ewma_drift) * 1e4;  // used in driftBps JSON field below
    const double projected_fp_bid = supp.fp_bid;
    const double projected_fp_ask = supp.fp_ask;

    // queue_ahead for any live resting orders — the ACTUAL tracked queue,
    // not the projected one. Used by the dashboard to show how buried the
    // current resting orders are.
    double bid_queue_ahead = 0.0;
    double ask_queue_ahead = 0.0;
    double bid_fill_prob = 0.0;
    double ask_fill_prob = 0.0;
    if (st.bid_order_id != 0) {
        if (const auto* e = st.queue.lookup(st.bid_order_id)) {
            bid_queue_ahead = e->queue_ahead;
            bid_fill_prob = st.queue.fill_probability(st.bid_order_id);
        }
    }
    if (st.ask_order_id != 0) {
        if (const auto* e = st.queue.lookup(st.ask_order_id)) {
            ask_queue_ahead = e->queue_ahead;
            ask_fill_prob = st.queue.fill_probability(st.ask_order_id);
        }
    }

    nlohmann::json j;
    j["type"] = "strategyState";
    // Discriminator for the dashboard's panel registry. Every strategy
    // that implements get_strategy_state_json() must set `kind`; the
    // frontend picks the matching panel component (panels/index.ts).
    j["kind"] = "AS";
    j["symbol"] = st.symbol;
    j["exchange"] = st.exchange;

    // Model parameters (live values, not config)
    j["drift"] = st.ewma_drift;
    j["driftBps"] = drift_bps;
    j["slowDriftBps"] = st.slow_drift_bps;
    j["slowDriftSuppressBps"] = slow_drift_suppress_bps_;
    j["sigma2"] = st.ewma_var;
    j["kappa"] = (st.kappa_ticks >= kappa_warmup_ticks_) ? std::max(kappa_min_, st.ewma_kappa) : kappa_;
    j["kappaLive"] = st.kappa_ticks >= kappa_warmup_ticks_;

    // Regime
    j["regime"] = st.regime.regime_name();
    j["hurst"] = st.regime.hurst();
    j["gammaBase"] = gamma_;
    const double gpnl_mult = gamma_pnl_mult(st);
    j["gammaEffective"] = gamma_ * st.regime.gamma_multiplier() * gpnl_mult;
    j["gammaMultiplier"] = st.regime.gamma_multiplier();
    j["gammaPnlMultiplier"] = gpnl_mult;
    j["gammaPnlWindow"] = static_cast<int>(gamma_pnl_window_n_);
    j["gammaPnlRecentSum"] = [&]() {
        double s = 0.0;
        for (double r : st.recent_rpnl) s += r;
        return s;
    }();

    // Quotes
    j["mid"] = st.last_mid;
    j["reservation"] = reservation;
    j["reservationOffsetBps"] = reservation_offset_bps;
    j["halfSpread"] = half_spread;
    j["halfSpreadBps"] = st.last_mid > 0 ? half_spread / st.last_mid * 1e4 : 0;

    // Inventory — report the EFFECTIVE cap (adaptive when configured),
    // not the static fallback, so the dashboard inventoryPct gauge
    // tracks the same threshold the strategy is actually enforcing.
    const double max_inv = effective_max_inventory(st);
    j["inventory"] = net_qty;
    j["maxInventory"] = max_inv;
    j["inventoryPct"] = max_inv > 0 ? std::abs(net_qty) / max_inv * 100.0 : 0;

    // Suppression state per side — priority ladder lives on the
    // SuppressionState struct (vol_gate → inventory → drift → trend →
    // tox → queue). Both the boolean and reason string come from the
    // same struct so they can never drift.
    j["bidSuppressed"] = supp.bid_suppressed();
    j["bidSuppressReason"] = std::string(supp.bid_reason());
    j["askSuppressed"] = supp.ask_suppressed();
    j["askSuppressReason"] = std::string(supp.ask_reason());

    // Vol gate
    j["volGateHalted"] = supp.vol_halted;
    j["volGateTrips"] = st.vol_gate.trips_total();

    // Orders
    j["bidOrderLive"] = st.bid_order_id != 0;
    j["askOrderLive"] = st.ask_order_id != 0;
    j["bidPrice"] = st.last_bid_price;
    j["askPrice"] = st.last_ask_price;

    // Warmup
    j["volTicks"] = st.ewma_ticks;
    j["volWarmup"] = vol_warmup_ticks_;
    j["warmedUp"] = st.ewma_ticks >= vol_warmup_ticks_;

    // Queue state — actual (for resting orders) and projected (for the
    // quote the strategy would place on the next tick).
    j["bookBidLevels"] = st.book.n_bid_levels();
    j["bookAskLevels"] = st.book.n_ask_levels();
    j["bidQueueAhead"] = bid_queue_ahead;
    j["askQueueAhead"] = ask_queue_ahead;
    j["bidFillProb"] = bid_fill_prob;
    j["askFillProb"] = ask_fill_prob;
    j["bidProjectedFillProb"] = projected_fp_bid;
    j["askProjectedFillProb"] = projected_fp_ask;
    j["queueSuppressMin"] = queue_suppress_fill_prob_min_;

    // Market best bid/ask — cached by on_bbo. Preferred over st.book
    // because this strategy runs with order_book_depth=0 (no L2 ladder
    // consumption); st.book.ready() would always return false here.
    j["marketBid"] = st.last_market_bid;
    j["marketAsk"] = st.last_market_ask;

    return j.dump();
}

// ── Shutdown flatten ────────────────────────────────────────────────────────

void AvellanedaStoikovStrategy::on_shutdown_flatten() {
    if (!order_mgr_) {
        bpt::common::log::warn(kLog(), "shutdown flatten: order_mgr null — cannot flatten");
        return;
    }

    int cancels = 0;
    int unwinds = 0;
    int cancel_alls = 0;

    // Exchange-authoritative sweep, fired FIRST before the tracked-order
    // cancels below. Routes via order-gateway → adapter.send_cancel_all,
    // which (on HL) queries /info openOrders and batch-cancels every
    // returned oid. Catches orphans our in-memory state never knew about
    // — WS reconnect, lost cancel-ack races, and (critically) orders
    // left behind by a previous session whose shutdown thought it had
    // nothing to cancel because bid_order_id / ask_order_id had already
    // been cleared locally. Individual cancels below stay — they provide
    // the has_pending_flatten() drain signal; cancel_all is fire-and-
    // forget from the strategy side (see strategy_app_shutdown for the
    // minimum drain window that gives the async round-trip time to land).
    for (const auto& [instrument_id, st] : state_) {
        bpt::common::log::warn(kLog(),
                               "SHUTDOWN FLATTEN {} {} cancel_all (exchange-authoritative sweep)",
                               st.symbol, st.exchange);
        order_mgr_->cancel_all(st.exchange_id, instrument_id);
        ++cancel_alls;
    }

    for (auto& [instrument_id, st] : state_) {
        // Cancel resting bid + ask so they don't interfere with the unwind
        // or get filled right as we're exiting.
        if (st.bid_order_id != 0) {
            bpt::common::log::warn(kLog(),
                                   "SHUTDOWN FLATTEN {} cancelling bid order_id={}",
                                   st.symbol,
                                   st.bid_order_id);
            order_mgr_->cancel_order(st.bid_order_id, st.exchange_id, instrument_id);
            st.bid_cancel_pending = true;
            ++cancels;
        }
        if (st.ask_order_id != 0) {
            bpt::common::log::warn(kLog(),
                                   "SHUTDOWN FLATTEN {} cancelling ask order_id={}",
                                   st.symbol,
                                   st.ask_order_id);
            order_mgr_->cancel_order(st.ask_order_id, st.exchange_id, instrument_id);
            st.ask_cancel_pending = true;
            ++cancels;
        }

        // Unwind any net inventory with an aggressive IOC cross.
        //
        // Position source priority: exchange's most-recent AccountSnapshot
        // if fresh (< 10 s old), else the strategy-side PositionTracker.
        // AccountSnapshot is exchange-authoritative so it absorbs any
        // fill-reporting lag / queue-drain race in the strategy-side
        // tracker at shutdown. Fallback to PositionTracker covers the
        // case where no snapshot has arrived yet (early session,
        // account-snapshot stream down).
        constexpr uint64_t kSnapshotFreshnessNs = 10ULL * 1'000'000'000ULL;
        const uint64_t now_wall_ns = bpt::strategy::clock::SimClock::now_ns();
        const bool snapshot_fresh = last_snapshot_ns_ > 0 && (now_wall_ns - last_snapshot_ns_) <= kSnapshotFreshnessNs;

        int64_t net_qty_e8 = 0;
        const char* qty_source = "tracker";
        if (snapshot_fresh) {
            const auto it = last_snapshot_qty_e8_.find({st.exchange_id, st.symbol});
            if (it != last_snapshot_qty_e8_.end()) {
                net_qty_e8 = it->second;
                qty_source = "snapshot";
            } else {
                net_qty_e8 = positions_.net_qty(instrument_id, st.exchange_id);
            }
        } else {
            net_qty_e8 = positions_.net_qty(instrument_id, st.exchange_id);
        }
        const double net_qty = static_cast<double>(net_qty_e8) / 1e8;
        if (net_qty == 0.0 || st.last_mid <= 0.0)
            continue;

        bpt::common::log::info(kLog(), "SHUTDOWN FLATTEN {} position_source={}", st.symbol, qty_source);

        const auto side = (net_qty > 0.0) ? OrderSide::SELL : OrderSide::BUY;
        bpt::common::log::warn(kLog(), "SHUTDOWN FLATTEN {} unwinding net_qty={:.8f} via IOC", st.symbol, net_qty);
        // Arm retry budget — consumed in on_exec_report on terminal
        // status of this unwind (rejected or partial+cancelled). Resets
        // to 0 once residual is flat or the budget is exhausted.
        st.unwind_retries_left = shutdown_max_unwind_retries_;
        // Mark this unwind as shutdown-originated so the EXHAUSTED
        // watchdog only fires for actual drain failures (not for the
        // normal-path inventory-cap unwinds that flow through the same
        // on_exec_report code path).
        st.unwind_is_shutdown_drain = true;
        send_unwind_order(instrument_id, st, side, st.last_mid, std::abs(net_qty));
        ++unwinds;
    }

    if (cancels > 0 || unwinds > 0 || cancel_alls > 0)
        bpt::common::log::warn(kLog(),
                               "shutdown flatten: cancel_all swept {} venue(s), cancelled {} tracked order(s), fired {} unwind IOC(s)",
                               cancel_alls,
                               cancels,
                               unwinds);
}

std::size_t AvellanedaStoikovStrategy::on_account_snapshot(bpt::messages::AccountSnapshot& snap) {
    // Step 1: drain the SBE positions group exactly once into a map.
    // Cached for shutdown flatten (exchange-authoritative position
    // source) AND re-used for the reconcile pass below — SBE group
    // cursors can only be walked once per message.
    //
    // ORDER MATTERS: SBE repeating groups share a read cursor with the
    // parent message, so positions must be drained before
    // currencyBalances. Both helpers are non-const on snap.
    //
    // Rewind the cursor up front — strategy_app's log line calls
    // snap.positions().count() before handing us the message, which
    // advances the group cursor past the positions header. Without
    // rewinding, extract_exchange_positions reads the currencyBalances
    // header as if it were the positions header (silent corruption on
    // the old code path; crash-or-garbage once we also call
    // currencyBalances here).
    snap.sbeRewind();

    // Cache exchange-reported total equity for equity-fraction sizing
    // (see effective_order_qty / effective_max_inventory). Captured here
    // so every AccountSnapshot refresh updates the sizing baseline; if
    // equity moves, next tick's quote sizes follow without operator
    // intervention. Quoted in USD-equivalent for HL perp; SPOT venues
    // need conversion that is out of scope for the single-instrument
    // PERP path this is currently exercised on.
    last_equity_e8_ = snap.totalEquityE8();

    const auto exchange_id = snap.exchangeId();
    // Row-level extract preserves avg entry price alongside qty so we
    // can seed PositionTracker on a divergence (see reconciler loop
    // below). Legacy exchange_by_symbol_raw map kept for reconcile() +
    // the shutdown-flatten cache which only want qty.
    const auto exchange_row_by_symbol = extract_exchange_position_rows(snap);
    std::unordered_map<std::string, int64_t> exchange_by_symbol_raw;
    exchange_by_symbol_raw.reserve(exchange_row_by_symbol.size());
    for (const auto& [symbol, row] : exchange_row_by_symbol) {
        exchange_by_symbol_raw[symbol] = row.net_qty_e8;
    }
    const auto currency_equity_e8 = extract_exchange_currency_balances(snap);
    for (const auto& [symbol, qty_e8] : exchange_by_symbol_raw) {
        last_snapshot_qty_e8_[{exchange_id, symbol}] = qty_e8;
    }
    last_snapshot_ns_ = snap.timestampNs();

    // On the first post-refdata snapshot we capture the session-start
    // currency baseline for SPOT reconciliation. PositionTracker was
    // zeroed in on_snapshot() so "delta from this baseline" == "net
    // traded according to the exchange". Subsequent snapshots use the
    // captured baseline to compute the SPOT delta.
    if (!initial_ccy_equity_captured_) {
        for (const auto& [ccy, equity] : currency_equity_e8) {
            initial_ccy_equity_e8_[{exchange_id, ccy}] = equity;
        }
        initial_ccy_equity_captured_ = true;
        bpt::common::log::info(kLog(),
                               "SPOT reconcile baseline captured: {} ccy row(s) on exchange={}",
                               currency_equity_e8.size(),
                               bpt::messages::ExchangeId::c_str(exchange_id));
    }

    // Step 2: build the map we'll hand to reconcile(). For PERP/FUTURE
    // the exchange view is positions[symbol]. For SPOT that row is
    // missing (or spuriously populated by quote-currency holdings), so
    // we override with delta = current_ccy_equity - initial_ccy_equity.
    std::unordered_map<std::string, int64_t> exchange_by_symbol = exchange_by_symbol_raw;

    // Step 3: build id→symbol map for our tracker-side entries on this
    // exchange. At the same time, rewrite SPOT entries to use the
    // delta-based exchange view computed from currency balances.
    std::unordered_map<uint64_t, std::string> symbol_map;
    symbol_map.reserve(state_.size());
    for (const auto& [id, st] : state_) {
        if (st.exchange_id != exchange_id)
            continue;
        symbol_map[id] = st.symbol;

        if (st.instrument_type == refdata::InstrumentType::SPOT && !st.base_ccy.empty()) {
            const auto it_cur = currency_equity_e8.find(st.base_ccy);
            const auto it_base = initial_ccy_equity_e8_.find({exchange_id, st.base_ccy});
            if (it_cur != currency_equity_e8.end() && it_base != initial_ccy_equity_e8_.end()) {
                exchange_by_symbol[st.symbol] = it_cur->second - it_base->second;
            } else {
                // Missing baseline or missing current row → we can't
                // compute a meaningful delta. Drop the symbol so the
                // reconciler treats it as "exchange didn't report" and
                // compares against 0 — which will fire iff tracker has
                // moved from 0. Acceptable: we only enter this branch
                // on the first snapshot before baseline capture, or if
                // the exchange stops reporting the ccy entirely.
                exchange_by_symbol.erase(st.symbol);
            }
        }
    }
    if (symbol_map.empty())
        return 0;  // nothing we care about on this exchange

    // Threshold: 1e4 in 1e8 scale = 0.0001 of a base unit (~$10 at
    // BTC prices, ~$0.40 at ETH prices). Smaller than the smallest
    // order_qty we place (0.0001 BTC); bigger than floating-point
    // rounding noise. Tune per-venue later if needed.
    constexpr int64_t kDivergenceThresholdE8 = 10000;  // 0.0001 base units

    const auto divergences = reconcile(positions_, exchange_by_symbol, exchange_id, symbol_map, kDivergenceThresholdE8);
    for (const auto& d : divergences) {
        bpt::common::log::warn(kLog(),
                               "RECONCILIATION DIVERGENCE instrument_id={} symbol='{}' "
                               "our_net_qty={:.8f} exchange_net_qty={:.8f} diff={:.8f}",
                               d.instrument_id,
                               d.exchange_symbol,
                               static_cast<double>(d.our_net_qty_e8) / 1e8,
                               static_cast<double>(d.exchange_net_qty_e8) / 1e8,
                               static_cast<double>(d.diff_e8) / 1e8);

        // Seed the tracker to the exchange view. The reconciler used
        // to only log, which silently left strategies quoting against
        // a stale inventory count across restarts — AS's inventory-
        // skew + max_inventory guards read a tracker saying "flat"
        // while the exchange had accumulated a real position from
        // prior sessions. See feedback_avoid_silent_divergence note
        // in project_prod_hardening_backlog.md.
        //
        // avg_entry_price is sourced from the same SBE row; for SPOT
        // symbols we derived qty from currency-balance delta rather
        // than a positions[] row, so there's no entry price to seed.
        // Pass 0.0 in that case — subsequent fills blend from 0-avg,
        // which produces wrong realized_pnl on any close of the
        // seeded portion but avoids fabricating an entry.
        double seed_avg_px = 0.0;
        if (const auto it = exchange_row_by_symbol.find(d.exchange_symbol);
            it != exchange_row_by_symbol.end()) {
            seed_avg_px = it->second.avg_entry_price;
        }
        positions_.seed(d.instrument_id, exchange_id, d.exchange_net_qty_e8, seed_avg_px);
        bpt::common::log::info(kLog(),
                               "reconciler: seeded position instrument_id={} symbol='{}' "
                               "to exchange view net_qty={:.8f} avg_price={:.4f}",
                               d.instrument_id, d.exchange_symbol,
                               static_cast<double>(d.exchange_net_qty_e8) / 1e8,
                               seed_avg_px);
    }
    return divergences.size();
}

bool AvellanedaStoikovStrategy::has_pending_flatten() const {
    // "Pending" = any instrument still has a resting bid, resting ask,
    // or in-flight unwind IOC. Each is cleared by on_exec_report on the
    // FILLED / CANCELLED / REJECTED terminal status, so this returns
    // false once the shutdown drain has processed the acks.
    for (const auto& [_, st] : state_) {
        if (st.bid_order_id != 0 || st.ask_order_id != 0 || st.unwind_order_id != 0)
            return true;
    }
    return false;
}

// ── Warm-start state ────────────────────────────────────────────────────────
//
// Schema version: bump on breaking format changes. Loader rejects files
// whose version doesn't match and falls back to cold start.
static constexpr int kWarmStartSchemaVersion = 1;

void AvellanedaStoikovStrategy::save_state(const std::string& path) {
    if (path.empty())
        return;

    try {
        nlohmann::json root;
        root["version"] = kWarmStartSchemaVersion;
        root["saved_at_ns"] = bpt::strategy::clock::SimClock::now_ns();

        nlohmann::json instruments = nlohmann::json::array();
        for (const auto& [instrument_id, st] : state_) {
            nlohmann::json j;
            j["instrument_id"] = instrument_id;
            j["symbol"] = st.symbol;
            j["exchange"] = st.exchange;
            j["ewma_var"] = st.ewma_var;
            j["ewma_ticks"] = st.ewma_ticks;
            j["last_mid"] = st.last_mid;
            j["last_tick_ns"] = st.last_tick_ns;
            j["ewma_drift"] = st.ewma_drift;
            j["ewma_kappa"] = st.ewma_kappa;
            j["kappa_ticks"] = st.kappa_ticks;
            j["last_trade_ns"] = st.last_trade_ns;

            const auto rs = st.regime.snapshot_state();
            nlohmann::json r;
            r["regime"] = static_cast<int>(rs.regime);
            r["hurst"] = rs.hurst;
            r["last_mid"] = rs.last_mid;
            r["returns"] = rs.returns;
            r["tick_count"] = rs.tick_count;
            j["regime"] = std::move(r);

            instruments.push_back(std::move(j));
        }
        root["instruments"] = std::move(instruments);

        // Atomic write: tmp + rename so a crash mid-serialise doesn't
        // leave a half-written file that the next boot would load.
        std::filesystem::path p(path);
        std::filesystem::create_directories(p.parent_path());
        const std::filesystem::path tmp = p.string() + ".tmp";
        {
            std::ofstream ofs(tmp);
            if (!ofs) {
                bpt::common::log::error(kLog(), "warm-start save: cannot open {} for write", tmp.string());
                return;
            }
            ofs << root.dump(2);
        }
        std::filesystem::rename(tmp, p);
        bpt::common::log::info(kLog(), "warm-start saved {} instrument(s) to {}", state_.size(), p.string());
    } catch (const std::exception& e) {
        bpt::common::log::error(kLog(), "warm-start save failed: {}", e.what());
    }
}

void AvellanedaStoikovStrategy::load_state(const std::string& path, uint64_t max_age_s) {
    if (path.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        bpt::common::log::info(kLog(), "warm-start: no prior state at {} — cold start", path);
        return;
    }

    try {
        std::ifstream ifs(path);
        nlohmann::json root = nlohmann::json::parse(ifs);

        const int version = root.value("version", 0);
        if (version != kWarmStartSchemaVersion) {
            bpt::common::log::warn(kLog(),
                                   "warm-start: schema mismatch (file={} expected={}) — cold start",
                                   version,
                                   kWarmStartSchemaVersion);
            return;
        }

        const uint64_t saved_at_ns = root.value<uint64_t>("saved_at_ns", 0);
        const uint64_t now_ns = bpt::strategy::clock::SimClock::now_ns();
        const uint64_t age_ns = (now_ns > saved_at_ns) ? (now_ns - saved_at_ns) : 0;
        const uint64_t max_age_ns = max_age_s * 1'000'000'000ULL;
        if (age_ns > max_age_ns) {
            bpt::common::log::warn(kLog(),
                                   "warm-start: saved state is {}s old (max {}s) — cold start",
                                   age_ns / 1'000'000'000ULL,
                                   max_age_s);
            return;
        }

        int restored = 0;
        int skipped = 0;
        for (const auto& j : root.value("instruments", nlohmann::json::array())) {
            const uint64_t instrument_id = j.value<uint64_t>("instrument_id", 0);
            auto it = state_.find(instrument_id);
            if (it == state_.end()) {
                ++skipped;  // instrument not in current universe
                continue;
            }
            auto& st = it->second;

            // Sanity: symbol + exchange must still match so a refdata
            // reshuffle doesn't silently graft OKX state onto a Binance
            // instrument that happens to have inherited the same id.
            const std::string saved_sym = j.value("symbol", "");
            const std::string saved_ex = j.value("exchange", "");
            if (saved_sym != st.symbol || saved_ex != st.exchange) {
                bpt::common::log::warn(kLog(),
                                       "warm-start: instrument_id={} symbol/exchange mismatch "
                                       "(saved '{}'/'{}' vs current '{}'/'{}') — skipping",
                                       instrument_id,
                                       saved_sym,
                                       saved_ex,
                                       st.symbol,
                                       st.exchange);
                ++skipped;
                continue;
            }

            st.ewma_var = j.value("ewma_var", 0.0);
            st.ewma_ticks = j.value<std::size_t>("ewma_ticks", 0);
            st.last_mid = j.value("last_mid", 0.0);
            st.last_tick_ns = j.value<uint64_t>("last_tick_ns", 0);
            st.ewma_drift = j.value("ewma_drift", 0.0);
            st.ewma_kappa = j.value("ewma_kappa", 0.0);
            st.kappa_ticks = j.value<std::size_t>("kappa_ticks", 0);
            st.last_trade_ns = j.value<uint64_t>("last_trade_ns", 0);

            if (auto r = j.find("regime"); r != j.end()) {
                RegimeDetector::StateSnapshot snap;
                snap.regime = static_cast<RegimeDetector::Regime>(r->value("regime", 0));
                snap.hurst = r->value("hurst", 0.5);
                snap.last_mid = r->value("last_mid", 0.0);
                snap.tick_count = r->value<std::size_t>("tick_count", 0);
                snap.returns = r->value("returns", std::vector<double>{});
                st.regime.restore_state(snap);
            }

            ++restored;
        }
        bpt::common::log::info(kLog(),
                               "warm-start: restored {} instrument(s) from {} (skipped {}, age {}s)",
                               restored,
                               path,
                               skipped,
                               age_ns / 1'000'000'000ULL);
    } catch (const std::exception& e) {
        bpt::common::log::error(kLog(), "warm-start load failed: {} — falling back to cold start", e.what());
    }
}

}  // namespace bpt::strategy::strategy
