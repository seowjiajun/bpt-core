#pragma once

// Quote-suppression policy: owns the suppression knobs and derives a
// per-side SuppressionState from per-instrument market state. Pure,
// read-only — no order I/O, no instrument mutation, no logging. Extracted
// from AvellanedaStoikovStrategy so the strategy holds one component instead
// of a dozen flat suppression scalars.

#include "features/fill_prob.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace bpt::strategy::strategy {

// Per-side suppression decision + the fill-prob diagnostics behind it.
// Single source of truth for runtime (maybe_requote) and console JSON.
//
// Organization: raw per-reason booleans for both sides, plus aggregate
// convenience accessors. Priority-ordered reason string comes out of the
// accessor so the string and the boolean can never drift.
struct SuppressionState {
    bool drift_bid{false}, drift_ask{false};            // per-√s EWMA drift
    bool trend_bid{false}, trend_ask{false};            // cumulative return over slow_drift_window_s
    bool tox_bid{false}, tox_ask{false};                // analytics toxicity score
    bool queue_bid{false}, queue_ask{false};            // projected fill-prob too low
    bool inventory_bid{false}, inventory_ask{false};    // |net_qty| >= max_inventory
    bool post_fill_bid{false}, post_fill_ask{false};    // post-fill markout cooldown (Phase 2.1)
    bool ofi_cancel_bid{false}, ofi_cancel_ask{false};  // OFI > θσ rule (research +0.915 bps/fill)
    bool vol_halted{false};                             // intra-tick realized-vol gate
    bool pause_active{false};                           // PnL drawdown circuit-breaker

    // Queue projection side outputs — populated when queue check runs,
    // cached here so logging + console don't recompute.
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
    // maybe_requote for the "cancel + don't replace" logic; the caller
    // checks inventory + vol_gate separately because it wants different log
    // strings for those cases.
    [[nodiscard]] bool bid_signal() const noexcept {
        return drift_bid || trend_bid || tox_bid || queue_bid || post_fill_bid || ofi_cancel_bid || pause_active;
    }
    [[nodiscard]] bool ask_signal() const noexcept {
        return drift_ask || trend_ask || tox_ask || queue_ask || post_fill_ask || ofi_cancel_ask || pause_active;
    }

    // Priority-ordered reason string. Priority: vol_gate → inventory →
    // post_fill → ofi_cancel → drift → trend → tox → queue (most to least
    // severe). post_fill ranks above ofi_cancel because it's a direct
    // response to a confirmed adverse fill, not a forecast; ofi_cancel ranks
    // above drift because OFI is current-tick book pressure, drift is a
    // per-√s rate estimate.
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

class SuppressionPolicy {
public:
    struct Config {
        // post_fill markout cooldown gate. <0 arms the per-side cooldown.
        double post_fill_markout_threshold_bps = 0.0;
        // Fast drift (per-√s |µ|) — fixed floor + σ-multiple adaptive companion.
        double drift_suppress_bps = 0.0;
        double drift_suppress_sigma_mult = 0.0;
        // Slow drift (cumulative return over the window) — floor + adaptive.
        double slow_drift_suppress_bps = 0.0;
        double slow_drift_suppress_sigma_mult = 0.0;
        double slow_drift_window_s = 300.0;  // also owned by the strategy (anchor advance)
        // Analytics toxicity — suppress side when score < threshold (<0; 0 disables).
        double tox_suppress_threshold = 0.0;
        // Queue position — suppress side when projected fill-prob < min.
        double queue_suppress_fill_prob_min = 0.0;
        double queue_suppress_horizon_s = 5.0;
        // κ estimator gate, shared with the pricer (queue Poisson formulation).
        double kappa_min = 0.01;
        std::size_t kappa_warmup_ticks = 10;
        // OFI cancel rule: |OFI| > θ·σ(OFI). +inf disables (default).
        double ofi_cancel_threshold_sigma = std::numeric_limits<double>::infinity();
    };

    SuppressionPolicy() = default;
    explicit SuppressionPolicy(Config cfg) : cfg_(cfg) {}

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

    // Compute the full suppression snapshot. Pure function of the instrument
    // state + inventory + candidate quote prices + the sizing inputs
    // (max_inv, eff_qty come from the sizer — suppression doesn't own sizing).
    // Templated on InstrumentState to avoid a header cycle with the strategy,
    // whose nested InstrumentState can't be forward-declared; instantiated in
    // the strategy TUs where the concrete type is visible.
    template <class InstrumentState>
    [[nodiscard]] SuppressionState evaluate(const InstrumentState& st,
                                            double net_qty,
                                            double new_bid,
                                            double new_ask,
                                            double max_inv,
                                            double eff_qty) const {
        SuppressionState s;

        // Inventory cap — hardest blocker (we never want to add beyond max).
        s.inventory_bid = net_qty >= max_inv;
        s.inventory_ask = net_qty <= -max_inv;

        // Phase 2.1 — per-side post-fill cooldown after an adverse fill.
        // Driven by the markout evaluation in on_bbo (writes
        // post_fill_suspend_until_*); 0 means no cooldown active. Compares
        // against st.last_tick_ns (simulation time) so backtest replays —
        // which compress 11h of sim time into a few seconds of wall clock —
        // honor the cooldown window correctly.
        if (cfg_.post_fill_markout_threshold_bps < 0.0) {
            s.post_fill_bid = st.post_fill_suspend_until_bid > 0 && st.last_tick_ns < st.post_fill_suspend_until_bid;
            s.post_fill_ask = st.post_fill_suspend_until_ask > 0 && st.last_tick_ns < st.post_fill_suspend_until_ask;
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
        // across assets and vol regimes. 0 when EWMA hasn't warmed; adaptive
        // part is then a no-op (threshold stays at the fixed floor).
        const double sigma_bps = st.ewma_var.value() > 0.0 ? std::sqrt(st.ewma_var.value()) * 1e4 : 0.0;

        // Drift (fast): suppress the adverse side when |µ| > threshold.
        // Threshold = max(fixed_floor, sigma_mult × σ_bps). With sigma_mult
        // = 3, this fires on ~3-SD-per-√s moves regardless of asset.
        const double drift_bps = std::abs(st.ewma_drift.value()) * 1e4;
        const double drift_threshold_bps =
            std::max(cfg_.drift_suppress_bps, cfg_.drift_suppress_sigma_mult * sigma_bps);
        const bool drift_on = drift_threshold_bps > 0.0 && drift_bps > drift_threshold_bps;
        s.drift_ask = drift_on && st.ewma_drift.value() > 0.0;  // uptrend → don't sell
        s.drift_bid = drift_on && st.ewma_drift.value() < 0.0;  // downtrend → don't buy

        // Trend (slow): keyed on cumulative return over slow_drift_window_s.
        // Expected stdev of a window-cumulative return ≈ σ × √window_s in
        // per-√s units, so the adaptive threshold multiplies σ_bps by the
        // window time-scale √window_s. Setting sigma_mult = 3 ≈ "3-SD
        // cumulative move over the window."
        const double trend_bps = std::abs(st.slow_drift_bps);
        const double trend_threshold_bps =
            std::max(cfg_.slow_drift_suppress_bps,
                     cfg_.slow_drift_suppress_sigma_mult * sigma_bps * std::sqrt(cfg_.slow_drift_window_s));
        const bool trend_on = trend_threshold_bps > 0.0 && trend_bps > trend_threshold_bps;
        s.trend_ask = trend_on && st.slow_drift_bps > 0.0;
        s.trend_bid = trend_on && st.slow_drift_bps < 0.0;

        // Toxicity: suppress a side when analytics reports its 5s markout
        // below threshold. Outcome-based (realized markout from our own
        // fills), complements drift's signal-based approach. Threshold is
        // negative; 0 disables.
        if (cfg_.tox_suppress_threshold < 0.0 && st.tox_data_received) {
            s.tox_bid = st.tox_bid_toxicity < cfg_.tox_suppress_threshold;
            s.tox_ask = st.tox_ask_toxicity < cfg_.tox_suppress_threshold;
        }

        // Queue position: project fill_prob at the candidate quote price
        // using the live ladder. Two formulations:
        //   - Time-aware (preferred): P(fill within T) ≈ 1 - exp(-κ·T / queue_ahead).
        //     Models taker arrival as a Poisson process at rate κ; fill
        //     happens when cumulative volume crosses queue_ahead. Gives a
        //     real probability with units of time, not just an ordinal ratio.
        //   - Geometric (fallback): our_qty / (our_qty + queue_ahead). Used
        //     when queue_suppress_horizon_s = 0 or κ hasn't warmed up.
        // Default fp = 1.0 when book isn't ready — don't suppress, quoting wins.
        if (cfg_.queue_suppress_fill_prob_min > 0.0 && st.book.ready()) {
            const double qa_bid = st.book.bid_vol_above(new_bid) + st.book.size_at_bid(new_bid);
            const double qa_ask = st.book.ask_vol_below(new_ask) + st.book.size_at_ask(new_ask);
            const bool kappa_warm = st.ewma_kappa.count() >= cfg_.kappa_warmup_ticks;
            if (cfg_.queue_suppress_horizon_s > 0.0 && kappa_warm) {
                const double kappa = std::max(cfg_.kappa_min, st.ewma_kappa.value());
                s.fp_bid = bpt::features::fill_probability_poisson(kappa, cfg_.queue_suppress_horizon_s, qa_bid);
                s.fp_ask = bpt::features::fill_probability_poisson(kappa, cfg_.queue_suppress_horizon_s, qa_ask);
            } else {
                s.fp_bid = bpt::features::fill_probability_geometric(eff_qty, qa_bid);
                s.fp_ask = bpt::features::fill_probability_geometric(eff_qty, qa_ask);
            }
            s.queue_bid = s.fp_bid < cfg_.queue_suppress_fill_prob_min;
            s.queue_ask = s.fp_ask < cfg_.queue_suppress_fill_prob_min;
        }

        // OFI cancel suppression — research-validated +0.915 bps/fill uplift
        // from per-fill markout sim across 5 spread regimes (see
        // bpt-research/findings/2026-05-24_ofi_cancel_spread_aware.md).
        // Suppress the side opposite the OFI signal when |OFI(t)| > θ × σ(OFI):
        //   OFI > +θσ (buy pressure) → suppress ASK (about to be lifted adversely)
        //   OFI < -θσ (sell pressure) → suppress BID (about to be hit adversely)
        // θ=inf disables (default); θ=0 cancels on any directional OFI.
        // 50-sample warmup on the EWMA of OFI² to avoid firing on early noise.
        if (!std::isinf(cfg_.ofi_cancel_threshold_sigma) && st.ewma_ofi_sq.count() > 50) {
            const double sigma_ofi = std::sqrt(st.ewma_ofi_sq.value());
            if (sigma_ofi > 0.0) {
                const double gate = cfg_.ofi_cancel_threshold_sigma * sigma_ofi;
                const double v = st.ofi.value();
                s.ofi_cancel_ask = v > gate;
                s.ofi_cancel_bid = v < -gate;
            }
        }

        return s;
    }

private:
    Config cfg_;
};

}  // namespace bpt::strategy::strategy
