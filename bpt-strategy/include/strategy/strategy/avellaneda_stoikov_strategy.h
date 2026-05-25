#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "features/ewma.h"
#include "features/fair_value.h"
#include "strategy/strategy/i_strategy.h"
#include "features/ofi.h"
#include "bpt_common/book/order_book_state.h"
#include "strategy/strategy/position_tracker.h"
#include "features/queue.h"
#include "features/regime_detector.h"
#include "features/vol_gate.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/OrderSide.h>

#include <atomic>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

using bpt::features::EwmaDrift;
using bpt::features::EwmaVariance;
using bpt::features::KappaEstimator;
using bpt::features::OFICalculator;
using bpt::features::TimeWeightedEwma;
using bpt::common::book::OrderBookState;
using bpt::features::FairValueEstimator;
using bpt::features::QueueTracker;
using bpt::features::RegimeDetector;
using bpt::features::VolatilityGate;

// Avellaneda-Stoikov market-making strategy.
//
// Per instrument, maintains two resting LIMIT GTC orders (one bid, one ask).
// On each BBO tick the strategy:
//
//   1. Estimates per-second realised volatility σ² via EWMA of squared
//      time-normalised log mid-price returns:
//        norm_ret  = ln(m_t / m_{t-1}) / sqrt(dt_s)
//        λ_t       = exp(-dt_s / vol_halflife_s)        (time-consistent decay)
//        σ²_t      = λ_t * σ²_{t-1} + (1 - λ_t) * norm_ret²
//      EWMA is O(1) state, has no cliff-edge roll-off, and lets older
//      observations decay smoothly.  Quoting begins after vol_warmup_ticks_
//      observations have been seen.
//
//   2. Computes the reservation (inventory-adjusted) price:
//        r = s - q * γ * σ² * (T - t)
//      where s = current mid, q = net inventory (base units, signed),
//      γ = risk aversion, T - t = remaining session seconds.
//
//   3. Computes the optimal half-spread:
//        δ/2 = γ * σ² * (T - t) / 2  +  (1/γ) * ln(1 + γ/κ)
//      where κ = market order arrival rate parameter.
//      δ/2 is clamped to at least max(min_half_spread_bps_, maker_fee_bps) × mid.
//
//   4. Posts bid = r - δ/2 and ask = r + δ/2, rounded away from mid to the
//      nearest instrument tick (bid floors, ask ceils).
//
//   5. Cancels and reposts a side when its price drifts beyond requote_threshold_.
//      Repricing waits for the CANCELLED confirmation before placing the new order
//      to avoid duplicate orders.
//
//   6. Suppresses new bids when net_qty ≥ max_inventory_ and new asks when
//      net_qty ≤ -max_inventory_, preventing inventory from growing further.
class AvellanedaStoikovStrategy : public IStrategy {
public:
    AvellanedaStoikovStrategy(uint64_t correlation_id,
                              const config::StrategyConfig& cfg,
                              refdata::IRefdataClient& refdata,
                              md::IMdClient* md,
                              order::OrderManager* order_mgr);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;
    void on_order_book(const bpt::messages::MdOrderBook& book) override;
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;
    std::size_t on_account_snapshot(bpt::messages::AccountSnapshot& snap) override;
    void on_toxicity_update(const bpt::analytics::messaging::ToxicityUpdate& update) override;
    void on_refdata_stale_changed(bool stale) override;
    std::string get_strategy_state_json() override;
    void on_shutdown_flatten() override;
    [[nodiscard]] bool has_pending_flatten() const override;

    // Persist per-instrument EWMA + regime state so a restart within
    // max_age_s doesn't eat the full vol/drift/kappa warmup.
    void save_state(const std::string& path) override;
    void load_state(const std::string& path, uint64_t max_age_s) override;

private:
    // OrderState::tag values used by AS — dispatched on in on_exec_report.
    static constexpr uint8_t kTagQuote = 0;            // resting bid/ask
    static constexpr uint8_t kTagUnwindNormal = 1;     // inventory-cap auto-unwind
    static constexpr uint8_t kTagUnwindShutdown = 2;   // shutdown-drain unwind

    struct InstrumentState {
        // EWMA estimators live in bpt-features. Halflives are passed via
        // designated init from on_snapshot/on_delta below.
        EwmaVariance ewma_var;   // per-second variance σ²
        EwmaDrift ewma_drift;    // per-√second drift µ
        KappaEstimator ewma_kappa;  // per-side trade arrival rate κ

        // Cached separately from the EWMAs because other AS code paths
        // (slow drift window, suppression cooldowns, console state) read
        // them outside the EWMA update flow.
        double last_mid{0.0};
        uint64_t last_tick_ns{0};
        uint64_t last_trade_ns{0};

        // Two-sided quoting: one resting order per side. OM owns lifecycle;
        // CancelPending status is read from the handle (replaces the old
        // bid_cancel_pending / ask_cancel_pending flags).
        order::OrderHandle h_bid;
        order::OrderHandle h_ask;

        // LIMIT IOC order to unwind inventory when it hits max_inventory_.
        // valid() while an unwind is in flight.
        order::OrderHandle h_unwind;

        // Remaining retry attempts if a shutdown-path unwind IOC is
        // rejected or partial-fills + cancels leaving residual position.
        // Non-zero only between on_shutdown_flatten() and drain
        // completion. Zero during normal operation so non-shutdown
        // unwinds (inventory-cap breaches) don't auto-retry.
        uint32_t unwind_retries_left{0};

        // Source-of-unwind (shutdown drain vs inventory-cap auto-unwind) is
        // encoded in h_unwind.state->tag (kTagUnwindShutdown / kTagUnwindNormal).

        // Prices of the currently live (or most recently placed) orders.
        // Used to detect whether the quote has drifted beyond requote_threshold_.
        double last_bid_price{0.0};
        double last_ask_price{0.0};

        // Most-recent market top-of-book, cached from on_bbo. Only used
        // by the console state publisher — the strategy math operates
        // on last_mid. Keeping it separate from st.book so strategies
        // configured with order_book_depth=0 still have a reference for
        // the chart overlay.
        double last_market_bid{0.0};
        double last_market_ask{0.0};

        // Mid price at the time each side was placed — used for directional cancel.
        // Cancel a bid if mid has risen by more than requote_threshold_ since placement
        // (informed flow is pushing against us); same logic inverted for asks.
        double bid_placed_mid{0.0};
        double ask_placed_mid{0.0};

        // Session start (ns) — used to compute T - t.
        uint64_t session_start_ns{0};

        // Exchange-error rejection backoff.
        // Consecutive EXCHANGE-sourced rejections trigger increasing cooldowns
        // (5s / 15s / 30s) to avoid flooding the exchange when the account has
        // no balance or is otherwise rejecting orders. Resets on ACKED.
        uint32_t consecutive_exchange_errors{0};
        uint64_t reject_backoff_until_ns{0};  // steady_clock ns; 0 = not in backoff

        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};

        // Set from refdata at on_snapshot(). Drives SPOT-vs-derivative
        // branching in the reconciler — SPOT holdings land in the
        // base-currency equity row on AccountSnapshot rather than the
        // positions[] group, so reconcile compares against a delta
        // from the session-start balance instead of an absolute.
        refdata::InstrumentType instrument_type{refdata::InstrumentType::SPOT};
        std::string base_ccy;  // base currency, e.g. "BTC" for "BTC-USDT"

        double tick_size{0.0};  // minimum price increment from refdata (0 = unknown)
        double lot_size{0.0};   // minimum quantity increment from refdata (0 = unknown)

        // Short-horizon realized-vol gate — blocks quoting during fast
        // moves so we don't get run over while requoting against stale
        // depth. See strategy/volatility_gate.h for semantics.
        VolatilityGate vol_gate;

        // Regime detector — classifies market as mean-reverting, trending,
        // or neutral using rolling Hurst exponent. Adjusts gamma dynamically.
        RegimeDetector regime;

        // Latest toxicity scores from Analytics (updated via on_toxicity_update).
        // NaN when no data. Used for side suppression alongside drift.
        double tox_bid_toxicity{0.0};
        double tox_ask_toxicity{0.0};
        bool tox_data_received{false};

        // Slow-drift (trend) tracking — cumulative return from a window-
        // start anchor. Catches multi-minute trends that the per-√s
        // drift EWMA misses because its per-second rate is too small
        // to clear the drift_suppress_bps threshold. Updated in on_bbo:
        // advance the window anchor once per slow_drift_window_s_,
        // recompute slow_drift_bps as (mid - anchor) / anchor in bps.
        // See suppression logic in compute_quotes.
        double slow_drift_window_start_mid{0.0};
        uint64_t slow_drift_window_start_ns{0};
        double slow_drift_bps{0.0};  // cached for state JSON + suppression check

        // Rolling per-fill realized-PnL deltas — fixed-size FIFO,
        // length capped by gamma_pnl_window_n_. Used by gamma_pnl_mult()
        // to widen γ after a recent loss streak / tighten after a
        // recent profit streak. Empty when the feature is disabled
        // (gamma_pnl_window_n_ == 0) or pre-fill warmup.
        std::deque<double> recent_rpnl;

        // ── Post-fill markout-based per-side cooldown (Phase 2.1) ───────────
        //
        // After each fill, record the fill price + timestamp. The next
        // BBO tick to arrive computes the markout — for a BUY fill this
        // is (mid_now - fill_price) / fill_price * 1e4 (positive = the
        // market moved up after our buy, favorable). For a SELL fill we
        // negate (positive = market moved down after our sell). If the
        // markout is below post_fill_markout_threshold_bps, we suspend
        // that side's quoting for post_fill_markout_cooldown_s — the
        // toxic-flow burst typically reverts within tens of seconds, so
        // staying out during it cuts adverse selection.
        //
        // pending_*_fill_price > 0 indicates a fill awaiting evaluation;
        // cleared once the next BBO tick computes the markout.
        double pending_buy_fill_price{0.0};
        uint64_t pending_buy_fill_ts{0};
        double pending_sell_fill_price{0.0};
        uint64_t pending_sell_fill_ts{0};

        // While now < post_fill_suspend_until_*, the corresponding side
        // is suppressed via SuppressionState. 0 means not suspended.
        uint64_t post_fill_suspend_until_bid{0};
        uint64_t post_fill_suspend_until_ask{0};

        // Maintained L2 ladder. Populated from MdOrderBook deltas in
        // on_order_book(); read by the queuing logic in Phase 3+.
        OrderBookState book;

        // Queue-position estimate per resting order at this instrument.
        // Populated on order placement (in send_limit_order), updated on
        // trade prints (in on_trade) and our own fills (in on_exec_report).
        QueueTracker queue;

        // Fair-value estimator providing the AS reservation-price `s`.
        // Default-constructs to Mode::kMid (no behavior change). The
        // strategy overwrites with the configured estimator after each
        // InstrumentState is inserted into state_.
        FairValueEstimator fv;

        // Order-Flow Imbalance signal — additive skew to the AS
        // reservation. Default-config gives a 1s rolling window over
        // 5 levels; strategy overwrites with the configured window/depth
        // after emplace, same pattern as `fv`. ofi_weight_bps_ = 0
        // (default) makes the signal a no-op so existing configs
        // produce byte-identical quotes.
        OFICalculator ofi{OFICalculator::Config{}};

        // Rolling σ²(OFI) for the OFI-cancel suppression threshold —
        // EWMA of OFI² with halflife = ofi_sigma_halflife_s_. Updated
        // alongside `ofi` in on_order_book only when the cancel rule
        // is armed (ofi_cancel_threshold_sigma_ > 0); skipped otherwise
        // to keep the hot path identical to the legacy baseline. The
        // halflife default in this in-class initializer is overwritten
        // by the designated-init in on_snapshot/on_delta.
        TimeWeightedEwma ewma_ofi_sq{60.0};
        uint64_t ewma_ofi_last_ns{0};

        // Reusable scratch buffers for the OFI block in on_order_book.
        // First call grows them to ladder_depth; subsequent ticks just
        // .clear() + refill without allocating. Keeping them per-instrument
        // (rather than thread_local) avoids spurious sharing if more than
        // one symbol is later quoted on the same thread.
        std::vector<OrderBookState::Level> ofi_top_bid_buf;
        std::vector<OrderBookState::Level> ofi_top_ask_buf;
        std::vector<OFICalculator::Level> ofi_bids_buf;
        std::vector<OFICalculator::Level> ofi_asks_buf;

        // Drawdown circuit-breaker. When set non-zero (in steady_clock ns),
        // both sides are suppressed until this timestamp passes. Set in
        // on_exec_report when realized PnL crosses the configured loss
        // threshold; cleared implicitly by the timestamp check (no resume
        // event needed). 0 = not paused.
        uint64_t pause_until_ns{0};
    };

    // Compute new bid/ask from the AS model.
    // Returns false if the volatility window is not yet warmed up.
    // Maker fee from FeeCache is added to the minimum half-spread floor so
    // the spread always covers the round-trip cost (2 × maker_bps).
    bool compute_quotes(const InstrumentState& st,
                        uint64_t instrument_id,
                        double net_qty,
                        double mid,
                        uint64_t timestamp_ns,
                        double& out_bid,
                        double& out_ask) const;

    // Evaluate whether each side needs a cancel+requote and act accordingly.
    void maybe_requote(uint64_t instrument_id,
                       InstrumentState& st,
                       double net_qty,
                       double mid,
                       double new_bid,
                       double new_ask);

    // Aggregated suppression state — one per-tick snapshot of every
    // reason a side might be blocked from quoting. Exists to keep
    // maybe_requote's runtime decisions and get_strategy_state_json's
    // console reporting in lockstep: prior to extraction, each path
    // had its own copy of the logic and a new reason (e.g. today's
    // trend_suppress) had to be added in two places or the console
    // would silently disagree with the actual decision.
    //
    // Organization: raw per-reason booleans for both sides, plus
    // aggregate convenience accessors. Priority-ordered reason string
    // comes out of the accessor so the string and the boolean can
    // never drift.
    struct SuppressionState {
        bool drift_bid{false}, drift_ask{false};          // per-√s EWMA drift
        bool trend_bid{false}, trend_ask{false};          // cumulative return over slow_drift_window_s
        bool tox_bid{false}, tox_ask{false};              // analytics toxicity score
        bool queue_bid{false}, queue_ask{false};          // projected fill-prob too low
        bool inventory_bid{false}, inventory_ask{false};  // |net_qty| >= max_inventory
        bool post_fill_bid{false}, post_fill_ask{false};  // post-fill markout cooldown (Phase 2.1)
        bool ofi_cancel_bid{false}, ofi_cancel_ask{false};  // OFI > θσ rule (research +0.915 bps/fill)
        bool vol_halted{false};                           // intra-tick realized-vol gate
        bool pause_active{false};                         // PnL drawdown circuit-breaker

        // Queue projection side outputs — populated when queue check
        // runs, cached here so logging + console don't recompute.
        double fp_bid{1.0}, fp_ask{1.0};

        // Full aggregate — every reason counted. Used by the console
        // bidSuppressed / askSuppressed flags.
        [[nodiscard]] bool bid_suppressed() const noexcept {
            return drift_bid || trend_bid || tox_bid || queue_bid || inventory_bid || post_fill_bid || ofi_cancel_bid ||
                   vol_halted || pause_active;
        }
        [[nodiscard]] bool ask_suppressed() const noexcept {
            return drift_ask || trend_ask || tox_ask || queue_ask || inventory_ask || post_fill_ask || ofi_cancel_ask ||
                   vol_halted || pause_active;
        }

        // "Signal-only" aggregate — drift/trend/toxicity/queue/post_fill/ofi_cancel. Used by
        // maybe_requote for the "cancel + don't replace" logic; the
        // caller checks inventory + vol_gate separately because it
        // wants different log strings for those cases.
        [[nodiscard]] bool bid_signal() const noexcept {
            return drift_bid || trend_bid || tox_bid || queue_bid || post_fill_bid || ofi_cancel_bid || pause_active;
        }
        [[nodiscard]] bool ask_signal() const noexcept {
            return drift_ask || trend_ask || tox_ask || queue_ask || post_fill_ask || ofi_cancel_ask || pause_active;
        }

        // Priority-ordered reason string. Priority: vol_gate →
        // inventory → post_fill → ofi_cancel → drift → trend → tox →
        // queue (most to least severe). post_fill ranks above ofi_cancel
        // because it's a direct response to a confirmed adverse fill,
        // not a forecast; ofi_cancel ranks above drift because OFI is
        // current-tick book pressure, drift is a per-√s rate estimate.
        [[nodiscard]] std::string_view bid_reason() const noexcept {
            if (pause_active)
                return "pause";
            if (vol_halted)
                return "vol_gate";
            if (inventory_bid)
                return "inventory";
            if (post_fill_bid)
                return "post_fill";
            if (ofi_cancel_bid)
                return "ofi_cancel";
            if (drift_bid)
                return "drift";
            if (trend_bid)
                return "trend";
            if (tox_bid)
                return "toxicity";
            if (queue_bid)
                return "queue";
            return "";
        }
        [[nodiscard]] std::string_view ask_reason() const noexcept {
            if (pause_active)
                return "pause";
            if (vol_halted)
                return "vol_gate";
            if (inventory_ask)
                return "inventory";
            if (post_fill_ask)
                return "post_fill";
            if (ofi_cancel_ask)
                return "ofi_cancel";
            if (drift_ask)
                return "drift";
            if (trend_ask)
                return "trend";
            if (tox_ask)
                return "toxicity";
            if (queue_ask)
                return "queue";
            return "";
        }
    };

    // Compute the full suppression snapshot. Pure function of the
    // instrument state + inventory + candidate quote prices; does not
    // mutate state or log. maybe_requote does its own info-level
    // logging of drift/trend/toxicity/queue triggers using the values on
    // the returned struct.
    [[nodiscard]] SuppressionState compute_suppression(const InstrumentState& st,
                                                       double net_qty,
                                                       double new_bid,
                                                       double new_ask) const;

    // Place a LIMIT IOC order at an aggressive price to unwind inventory.
    // `tag` is kTagUnwindNormal for inventory-cap unwinds, kTagUnwindShutdown
    // when called from on_shutdown_flatten.
    order::OrderHandle send_unwind_order(uint64_t instrument_id,
                                         InstrumentState& st,
                                         bpt::messages::OrderSide::Value side,
                                         double mid,
                                         double qty,
                                         uint8_t tag);

    order::OrderHandle send_limit_order(uint64_t instrument_id,
                                        InstrumentState& st,
                                        bpt::messages::OrderSide::Value side,
                                        double price,
                                        double qty);

    uint64_t correlation_id_;

    // Model parameters
    double gamma_;                    // risk aversion (γ)
    double kappa_;                    // fallback κ used before EWMA estimate warms up
    double session_duration_s_;       // trading session length T (seconds)
    double vol_halflife_s_;           // EWMA half-life for σ² estimation (seconds)
    std::size_t vol_warmup_ticks_;    // min BBO ticks before quoting begins
    double kappa_halflife_s_;         // EWMA half-life for κ estimation (seconds)
    std::size_t kappa_warmup_ticks_;  // min trade ticks before live κ replaces fallback
    double kappa_min_;                // floor on κ to prevent ln(1 + γ/κ) blowing up
    double requote_threshold_;        // fractional price move that triggers a requote
    double max_inventory_;            // max |net position| in base units (fixed fallback)
    double order_qty_;                // quote size in natural units (e.g. 0.001 BTC, fixed fallback)
    // Equity-fraction sizing — adaptive companions to the fixed values
    // above. When set (>0), strategy derives actual sizes from the
    // exchange-reported totalEquity each tick:
    //   order_qty     = max(order_qty_min_,
    //                       order_qty_fraction_ × equity_usd / mid)
    //   max_inventory = max_inventory_fraction_ × equity_usd / mid
    // Lets one config target multiple capital scales + asset prices
    // without re-hand-sizing. 0 on either fraction disables the
    // adaptive part for that knob; fallback is the fixed value.
    //
    // order_qty_min_ is an absolute base-unit floor — prevents a shrinking
    // equity trajectory from driving qty to "unfillable by lot-size" and
    // stranding the position. Applied only when order_qty_fraction_ > 0.
    double order_qty_fraction_;
    double order_qty_min_;
    double max_inventory_fraction_;
    double min_half_spread_bps_;  // floor on half-spread expressed in basis points
    // Sanity clamp: hard ceiling on the half-spread the AS formula
    // can produce. Guards against the cold-start pathology where a
    // noisy early σ² estimate or κ underestimate pushes the formula
    // to quote 15x+ wider than the market. When clamped, we log at
    // WARN with the formula value + the applied clamp so operators
    // can see that warmup isn't done yet — better than silently
    // quoting wide.
    double max_half_spread_bps_;  // ceiling on half-spread in basis points
    // Sanity range around mid that any emitted quote must fall inside.
    // Last line of defence: if the AS formula produces a bid or ask
    // outside [mid * (1 - bps/10000), mid * (1 + bps/10000)] — typically
    // because the dimensional units in the inventory penalty break on
    // cheap instruments, or warmup is wildly off — skip the tick rather
    // than emitting nonsense to the OrderManager. Tracked separately
    // from max_half_spread_bps_ because that one bounds the FORMULA's
    // half-spread; this one bounds the FINAL quote level after all
    // adjustments (reservation skew, drift, post-touch cap).
    double quote_sanity_bps_;
    uint8_t order_book_depth_;  // 0 = BBO only, >0 subscribes to L2 ladder

    // Fair-value estimator config — drives the choice of AS reference
    // price `s`. Parsed from [fair_value] TOML table; defaults to
    // Mode::kMid which preserves the pre-estimator behavior.
    FairValueEstimator::Config fv_cfg_;

    // Drawdown circuit-breaker. When realized PnL drops below
    // pause_below_rpnl_usd_ (negative number, e.g. -0.50), AS pauses
    // both sides for pause_cooldown_s_. Resumes implicitly when the
    // pause timestamp passes. 0 disables. Per-instrument: only the
    // instrument that breached pauses; others continue.
    double pause_below_rpnl_usd_;
    double pause_cooldown_s_;

    // Phase 2.1 — per-side post-fill cooldown (adverse-selection defense).
    // After a fill, the next BBO tick computes its markout. If markout
    // is below post_fill_markout_threshold_bps, the corresponding side
    // is suspended for post_fill_markout_cooldown_s seconds. The toxic
    // burst that picked us off typically reverts within tens of seconds;
    // staying out during it cuts adverse selection at the cost of
    // missing some valid passive fills. Threshold is in bps of the
    // fill price; sign convention is "positive = favorable for us"
    // (so threshold should be a small negative number, e.g. -10.0).
    // Set threshold to 0 (or positive) to disable.
    double post_fill_markout_threshold_bps_;
    double post_fill_markout_cooldown_s_;

    // Drift (momentum) detection — Cartea-Jaimungal extension of AS.
    double drift_halflife_s_;  // EWMA half-life for µ estimation (seconds)
    // Warmup gate for drift_skew_frac. During the first N BBO ticks the
    // ewma_drift estimate is noisy — at session start, with no history,
    // a single big tick can produce drift_skew_frac of 30+ bps which
    // pushes reservation through the touch (POST_ONLY rejected, GTC
    // crosses). Suppress the drift contribution to reservation pricing
    // until the estimator has settled. Drift suppression / suppression
    // checks elsewhere remain unaffected — only the bid/ask offset.
    std::size_t drift_warmup_ticks_;
    // Hard cap on |drift_skew_frac| applied to reservation pricing. The
    // un-capped formula `ewma_drift × √(T-t)` amplifies even moderate
    // drift estimates by √session_duration (~60× for a 1h session at
    // start) — turning a 1 bp/√s drift into a 60 bp reservation skew
    // that drives quotes deep into the book. Cap defaults to 10 bps;
    // 0 disables the cap (legacy behaviour).
    double max_drift_skew_bps_;
    double drift_suppress_bps_;  // suppress adverse side when |µ| > this (bps/√s, fixed floor)
    // σ-multiple adaptive companion to drift_suppress_bps_. Effective
    // threshold at runtime = max(drift_suppress_bps_,
    //                            drift_suppress_sigma_mult_ × σ_ewma_bps).
    // Makes the knob asset-independent: tune once as "k SDs of realized
    // vol" rather than re-tuning bps for each venue / vol regime.
    // 0 disables adaptive part (purely fixed threshold).
    double drift_suppress_sigma_mult_;

    // Slow-drift (trend) detection — complements the per-√s EWMA drift
    // above. The fast signal is tuned for flash moves (threshold in
    // bps/√s, sensitive to sudden spikes). It misses sustained slow
    // bleeds — a 2% decline over 30 min on XMR registers as ~0.1 bps/√s,
    // well below drift_suppress_bps_ = 5, so the fast signal's
    // side-suppression never fires against a directional grind.
    //
    // Mechanism: anchor a mid sample at `window_start`, advance the
    // anchor every slow_drift_window_s_ seconds, compute
    // `slow_drift_bps = (mid_now - anchor) / anchor * 1e4`. Suppress
    // the trend-adverse side when |slow_drift_bps| > slow_drift_suppress_bps_.
    // Window-based (not EWMA) because the cumulative-return framing gives
    // a predictable threshold in absolute bps; EWMA would require an
    // adaptive threshold keyed to halflife.
    double slow_drift_window_s_;      // rolling anchor window (seconds)
    double slow_drift_suppress_bps_;  // suppress when |cum_return| > this (bps, fixed floor). 0 disables.
    // σ-multiple adaptive companion. Effective threshold at runtime =
    //   max(slow_drift_suppress_bps_,
    //       slow_drift_suppress_sigma_mult_ × σ_ewma_bps × √window_s).
    // The √window_s factor converts per-√s σ into a "typical cumulative
    // return magnitude over `window_s` seconds" — i.e., the expected
    // stdev of the cumulative-return measure we're thresholding. Setting
    // sigma_mult = 3 ≈ "fire on 3-SD cumulative moves over the window."
    // 0 disables adaptive part.
    double slow_drift_suppress_sigma_mult_;

    // Analytics toxicity suppression — suppress side when toxicity score < threshold.
    // 0 disables. Typical value: -2.0 (suppress when 5s markout is -2bps or worse).
    double tox_suppress_threshold_;  // negative value; 0 disables

    // Queue-position suppression — suppress a side if the projected
    // fill probability at the candidate quote price drops below this
    // floor. 0 disables.
    //
    // Two formulations:
    //   - Time-aware (preferred): P(fill within queue_suppress_horizon_s_) ≈
    //       1 - exp(-κ × T / queue_ahead). Uses the κ estimate AS already
    //       maintains. Threshold becomes a real probability ("P(fill in
    //       5s) < min") rather than an ordinal ratio.
    //   - Geometric (fallback): our_qty / (our_qty + queue_ahead). Used
    //       when queue_suppress_horizon_s_ = 0 or κ has not warmed up.
    double queue_suppress_fill_prob_min_;
    double queue_suppress_horizon_s_;

    // Shutdown-unwind aggression, expressed in basis points through
    // mid. Used by send_unwind_order() to price the IOC that flattens
    // residual inventory at shutdown. Default 20 bps is enough to cross
    // major-pair spreads in normal regimes; raise for thinner venues or
    // volatile markets where 20 bps no-fills and positions leak past
    // the drain budget. Retry logic re-reads this value on each attempt.
    double shutdown_cross_bps_;

    // Max retry attempts per instrument if the initial shutdown unwind
    // IOC is rejected or leaves residual position (partial fill then
    // IOC remainder cancels). Each retry refetches st.last_mid so the
    // IOC is priced against a current BBO. Total attempts per
    // instrument = 1 initial + shutdown_max_unwind_retries_. Default 3
    // gives ~4 attempts against the 5s post-drain budget.
    uint32_t shutdown_max_unwind_retries_;

    // Regime detector config — applied per-instrument at snapshot time.
    RegimeDetector::Config regime_cfg_;

    // Volatility gate config — applied per-instrument at snapshot time.
    VolatilityGate::Config vol_gate_cfg_;
    // σ-multiple adaptive companion to vol_gate_cfg_.max_bps_per_window.
    // Effective trip threshold at runtime =
    //   max(vol_gate_cfg_.max_bps_per_window,
    //       vol_gate_sigma_mult_ × σ_ewma_bps × √window_s).
    // Updated into st.vol_gate via set_max_bps_per_window() on every
    // on_bbo tick once the vol EWMA has warmed up. 0 disables adaptive
    // part (purely fixed threshold).
    double vol_gate_sigma_mult_;

    // γ feedback from recent realized PnL — closes a feedback loop on
    // the risk-aversion parameter using observed session performance.
    // Every FILLED/PARTIAL exec report pushes its rpnl delta into
    // st.recent_rpnl (FIFO, capped at gamma_pnl_window_n_);
    // gamma_pnl_mult(st) inspects the rolling sum and returns:
    //   gamma_pnl_widen_mult_   if sum < loss_threshold_usd  (loss streak → wider quotes)
    //   gamma_pnl_tighten_mult_ if sum > profit_threshold_usd (profit streak → tighter)
    //   1.0 otherwise (feature disabled or rpnl in the deadband)
    // The multiplier is folded into AS's effective γ alongside the
    // regime detector's gamma_multiplier.
    //
    // Defaults: window_n = 0 → feature disabled. This is the highest-
    // blast-radius adaptive knob on AS — a mis-tuned widen_mult of 5x
    // could lock the strategy into permanent wide quotes. Conservative
    // values (widen ≤ 2.0, tighten ≥ 0.7) recommended.
    std::size_t gamma_pnl_window_n_;
    double gamma_pnl_loss_threshold_usd_;
    double gamma_pnl_profit_threshold_usd_;
    double gamma_pnl_widen_mult_;
    double gamma_pnl_tighten_mult_;

    [[nodiscard]] double gamma_pnl_mult(const InstrumentState& st) const;

    // OFI quote-skew (Cont-Kukanov-Stoikov). Adds an additive offset
    // to the AS reservation price proportional to the rolling normalized
    // OFI signal:  reservation += mid * (ofi_weight_bps * 1e-4 * ofi_value).
    // Positive OFI lifts reservation (makes asks easier to fill, bids
    // harder), matching the drift-extension sign convention. Default 0
    // makes the feature a no-op so legacy configs are unchanged.
    double ofi_weight_bps_;
    uint64_t ofi_window_ns_;

    // OFI cancel suppression — research-validated +0.915 bps/fill uplift
    // from per-fill markout sim across 5 spread regimes (see
    // research/findings/2026-05-24_ofi_cancel_spread_aware.md).
    // When |OFI(t)| > ofi_cancel_threshold_sigma_ × σ(OFI), suppress the
    // side that would be adversely picked off:
    //   OFI > +θσ (buy pressure) → suppress ASK
    //   OFI < -θσ (sell pressure) → suppress BID
    // Threshold range: [0, inf]. θ=0 cancels on ANY directional OFI
    // (most aggressive, matches research script's best setting); larger
    // θ tightens the gate; θ=inf disables the rule (default — TOML omitted
    // resolves to inf via value_or). σ(OFI) is estimated by EWMA of OFI²
    // with halflife ofi_sigma_halflife_s_, updated on every OFI tick.
    double ofi_cancel_threshold_sigma_;
    double ofi_sigma_halflife_s_;

    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::IRefdataClient& refdata_;
    md::IMdClient* md_client_;
    order::OrderManager* order_mgr_;
    std::unordered_map<uint64_t, InstrumentState> state_;  // keyed by instrument_id
    PositionTracker positions_;

    // Exchange-authoritative position cache, keyed by (exchange_id,
    // exchange_symbol). Populated on every on_account_snapshot() arrival.
    // Preferred over PositionTracker at shutdown flatten time because
    // the strategy-side tracker can race a fill-reporting lag at
    // shutdown (gateway drops, exec-report queue drains slowly),
    // whereas AccountSnapshot reflects whatever the exchange booked
    // before we tore down the connection.
    struct SnapshotKey {
        bpt::messages::ExchangeId::Value exchange_id;
        std::string symbol;
        bool operator==(const SnapshotKey& o) const noexcept {
            return exchange_id == o.exchange_id && symbol == o.symbol;
        }
    };
    struct SnapshotKeyHash {
        std::size_t operator()(const SnapshotKey& k) const noexcept {
            return std::hash<std::string>{}(k.symbol) ^
                   (static_cast<std::size_t>(k.exchange_id) * 0x9E3779B97F4A7C15ULL);
        }
    };
    std::unordered_map<SnapshotKey, int64_t, SnapshotKeyHash> last_snapshot_qty_e8_;
    uint64_t last_snapshot_ns_{0};

    // Session-start currency equity baseline per (exchange_id, ccy).
    // SPOT reconciliation works by delta: exchange has moved us by
    // (current_equity - initial_equity), and we compare that to
    // PositionTracker.net_qty. Captured once on the first
    // AccountSnapshot that arrives after on_snapshot() (refdata ready),
    // at which point PositionTracker is guaranteed 0 (see on_snapshot
    // body). If the operator deposits/withdraws the base ccy mid-session
    // this baseline goes stale — accepted limitation; manual
    // intervention already breaks P&L accounting elsewhere.
    struct CcyKey {
        bpt::messages::ExchangeId::Value exchange_id;
        std::string ccy;
        bool operator==(const CcyKey& o) const noexcept { return exchange_id == o.exchange_id && ccy == o.ccy; }
    };
    struct CcyKeyHash {
        std::size_t operator()(const CcyKey& k) const noexcept {
            return std::hash<std::string>{}(k.ccy) ^ (static_cast<std::size_t>(k.exchange_id) * 0x9E3779B97F4A7C15ULL);
        }
    };
    std::unordered_map<CcyKey, int64_t, CcyKeyHash> initial_ccy_equity_e8_;
    bool initial_ccy_equity_captured_{false};

    // Latest exchange-reported total equity, in USD-equivalent quote
    // currency units (e8 fixed-point from AccountSnapshot.totalEquityE8).
    // Consumed only when equity-fraction sizing is enabled — zero means
    // "not yet received", and the fixed fallback order_qty / max_inventory
    // are used until the first snapshot arrives. Refreshed on every
    // on_account_snapshot call (bpt-order-gateway polls every 5s).
    int64_t last_equity_e8_{0};

    // Strategy-wide refdata-staleness flag, set by StrategyService via
    // on_refdata_stale_changed(). When true, on_bbo / on_order_book
    // skip new-quote computation early (existing cancels still flow)
    // because fee_cache.get() will return nullopt without a fresh
    // heartbeat, and quoting without a fee buffer is a silent bleed
    // against taker fees on every fill. Cleared automatically when
    // the heartbeat resumes; no operator intervention required.
    bool refdata_stale_{false};

    // Resolve the per-instrument order quote size. Returns adaptive
    // value when order_qty_fraction_ > 0 and equity + mid are known,
    // else the fixed order_qty_. Floor at order_qty_min_ when adaptive.
    [[nodiscard]] double effective_order_qty(const InstrumentState& st) const;

    // Resolve the per-instrument max inventory cap. Same pattern as
    // above; no explicit floor since a shrunk cap naturally restricts
    // new quotes via the existing inventory-suppression path.
    [[nodiscard]] double effective_max_inventory(const InstrumentState& st) const;
};

}  // namespace bpt::strategy::strategy
