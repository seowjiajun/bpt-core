// AS pricing + sizing + suppression math. No order I/O.

#include "strategy/strategy/avellaneda_stoikov_strategy.h"
#include "strategy/venue/min_order_value.h"

#include <algorithm>
#include <bpt_common/logging.h>
#include <cmath>

namespace bpt::strategy::strategy {

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("AS");
    return l;
}
}  // namespace

auto AvellanedaStoikovStrategy::compute_quotes(const InstrumentState& st, const BboContext& ctx) const
    -> std::optional<QuoteTarget> {
    if (st.ewma_var.count() < vol_warmup_ticks_)
        return std::nullopt;
    if (st.ewma_var.value() <= 0.0)
        return std::nullopt;

    const double net_qty = ctx.net_qty;
    const double mid = ctx.mid;
    const uint64_t timestamp_ns = ctx.ts_ns;

    // Remaining session time — clamp to [0, session_duration_s_].
    // After the session ends we keep quoting at the minimum spread (T-t = 0).
    const double elapsed_s = static_cast<double>(timestamp_ns - st.session_start_ns) * 1e-9;
    const double T_minus_t = std::max(0.0, session_duration_s_ - elapsed_s);

    const double sigma_sq = st.ewma_var.value();
    // Regime-adjusted gamma: widen spreads in trending regimes, tighten
    // in mean-reverting regimes. The multiplier comes from the Hurst-based
    // regime detector (1.8x in trending, 0.6x in mean-reverting, 1.0x neutral).
    // Effective γ folds in two adaptive factors:
    //   1. Regime detector multiplier (mean-rev / neutral / trending)
    //   2. PnL feedback multiplier (widen on recent loss streak)
    // Both default to 1.0 when their respective features are disabled,
    // so static γ behavior is unchanged unless operator opts in.
    const double effective_gamma = gamma_ * st.regime.gamma_multiplier() * gamma_pnl_mult(st);
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
    const double q_normalized = (max_inventory_ > 0.0) ? std::clamp(net_qty / max_inventory_, -1.0, 1.0) : 0.0;
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
    double drift_skew_frac =
        (st.ewma_drift.count() >= drift_warmup_ticks_) ? st.ewma_drift.value() * std::sqrt(T_minus_t) : 0.0;
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
    // Book-imbalance skew — L1 queue imbalance (bid_qty − ask_qty)/(bid_qty + ask_qty)
    // leans the reservation toward the predicted short-horizon move. Same sign as
    // OFI: bid-heavy (imb > 0) lifts reservation. Guard the 0/0 case (empty book,
    // e.g. order_book_depth=0). imbalance_weight_bps_ = 0 (default) → no-op.
    double book_imbalance_skew_frac = 0.0;
    if (imbalance_weight_bps_ != 0.0) {
        const double bq = st.book.best_bid_qty();
        const double aq = st.book.best_ask_qty();
        const double denom = bq + aq;
        if (denom > 0.0)
            book_imbalance_skew_frac = imbalance_weight_bps_ * 1e-4 * (bq - aq) / denom;
    }
    const double reservation =
        mid * (1.0 + drift_skew_frac + ofi_skew_frac + book_imbalance_skew_frac - inventory_skew_frac);
    // Kept for the debug log below; same as drift_skew_frac * mid.
    const double drift_adjustment = drift_skew_frac * mid;

    // Minimum half-spread: config floor + round-trip maker fee so we never
    // quote a spread that is guaranteed to lose money to commissions.
    // fee_half = maker_bps / 10000 * mid (one leg); both legs = 2x, so each
    // side of the spread must cover at least 1x maker fee.
    double fee_half_spread = 0.0;
    const auto fee_entry = refdata_.fee_cache().get(st.exchange_id, st.instrument_id, timestamp_ns);
    if (fee_entry) {
        fee_half_spread = (static_cast<double>(fee_entry->maker_bps) / 10000.0) * mid;
    }

    // Use live EWMA κ once warmed up; fall back to config kappa_ before then.
    // Floor at kappa_min_ to prevent ln(1 + γ/κ) → ∞ as κ → 0.
    const double kappa =
        (st.ewma_kappa.count() >= kappa_warmup_ticks_) ? std::max(kappa_min_, st.ewma_kappa.value()) : kappa_;

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
            bpt::common::log::warn(kLog(),
                                   "half-spread clamp: formula={:.2f} bps → clamped to {:.2f} bps "
                                   "(σ²={:.2e} κ={:.4f} ticks={} {}; {} clamps so far)",
                                   raw_half_spread / mid * 10000,
                                   max_half_spread_bps_,
                                   sigma_sq,
                                   kappa,
                                   st.ewma_var.count(),
                                   (st.ewma_var.count() < vol_warmup_ticks_ * 3) ? "WARMUP" : "σ-SPIKE",
                                   clamp_count);
        }
    }

    double bid = reservation - half_spread;
    double ask = reservation + half_spread;

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
        if (bid > bid_cap)
            bid = bid_cap;
        if (ask < ask_floor)
            ask = ask_floor;
        // Defensive: if the clamp inverts the spread (only possible on
        // a crossed market, which shouldn't happen but might in
        // transient feed states), treat as "don't quote this tick."
        if (bid >= ask)
            return std::nullopt;
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
        if (bid < lo || ask > hi || bid <= 0.0) {
            static std::size_t skip_count = 0;
            if (++skip_count <= 5 || skip_count % 1000 == 0) {
                bpt::common::log::warn(kLog(),
                                       "{} quote out of sanity range — skipping tick: "
                                       "bid={:.6f} ask={:.6f} mid={:.6f} reservation={:.6f} "
                                       "half_spread={:.6f} (sanity_bps={:.1f}; {} skips so far)",
                                       st.symbol,
                                       bid,
                                       ask,
                                       st.last_mid,
                                       reservation,
                                       half_spread,
                                       quote_sanity_bps_,
                                       skip_count);
            }
            return std::nullopt;
        }
    }

    bpt::common::log::debug(
        kLog(),
        "quotes σ²={:.2e} µ={:.2e} κ={:.4f} ({}) half_spread={:.4f} reservation={:.2f} drift_adj={:.4f}",
        sigma_sq,
        st.ewma_drift.value(),
        kappa,
        (st.ewma_kappa.count() >= kappa_warmup_ticks_) ? "live" : "fallback",
        half_spread,
        reservation,
        drift_adjustment);

    return QuoteTarget{bid, ask};
}

// ── Effective sizing — adaptive vs fixed ───────────────────────────────────
double AvellanedaStoikovStrategy::effective_order_qty(const InstrumentState& st) const {
    double qty = order_qty_;
    if (order_qty_fraction_ > 0.0 && last_equity_e8_ > 0 && st.last_mid > 0.0) {
        const double equity_usd = static_cast<double>(last_equity_e8_) / 1e8;
        const double derived = order_qty_fraction_ * equity_usd / st.last_mid;
        qty = std::max(order_qty_min_, derived);
    }
    // Bump qty up if the venue enforces a min-notional floor that the
    // chosen qty (fixed or equity-fractional) doesn't clear. HL has a
    // venue-wide $10 floor; without this, equity-fractional sizing on
    // a small account would emit orders that always reject.
    return bpt::strategy::venue::bump_qty_for_min_notional(qty,
                                                           st.last_mid,
                                                           st.lot_size,
                                                           bpt::strategy::venue::min_notional_usd(st.exchange));
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
    for (double r : st.recent_rpnl)
        sum += r;
    if (sum < gamma_pnl_loss_threshold_usd_)
        return gamma_pnl_widen_mult_;
    if (sum > gamma_pnl_profit_threshold_usd_)
        return gamma_pnl_tighten_mult_;
    return 1.0;
}

// Suppression policy (per-side cancel/requote decision) moved to
// suppression_policy.h — owned by supp_policy_, called from maybe_requote
// and get_strategy_state_json via supp_policy_.evaluate(...).

}  // namespace bpt::strategy::strategy
