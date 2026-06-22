#pragma once

// Avellaneda-Stoikov reservation-price calculator.
// Templated on InstrumentState to avoid a header cycle with the strategy;
// instantiated in the strategy TUs where the concrete type is visible.
// fee_half_spread is resolved by the caller so the pricer has no refdata dependency.

#include <algorithm>
#include <bpt_common/logging.h>
#include <cmath>
#include <cstddef>
#include <optional>

namespace bpt::strategy::strategy {

struct BboContext {
    double net_qty;
    double mid;
    uint64_t ts_ns;
};

struct QuoteTarget {
    double bid;
    double ask;
};

class ASPricer {
public:
    struct Config {
        double gamma{0.1};
        double kappa{1.5};                    // fallback κ before estimator warms
        double session_duration_s{86400.0};
        double max_inventory{0.1};            // for q_normalized γ-scaling
        std::size_t vol_warmup_ticks{20};
        std::size_t kappa_warmup_ticks{10};
        double kappa_min{0.01};
        std::size_t drift_warmup_ticks{50};
        double min_half_spread_bps{1.0};
        double max_half_spread_bps{50.0};
        double quote_sanity_bps{5000.0};
        double max_drift_skew_bps{10.0};
        double ofi_weight_bps{0.0};
        double imbalance_weight_bps{0.0};
        std::size_t gamma_pnl_window_n{0};
        double gamma_pnl_loss_threshold_usd{0.0};
        double gamma_pnl_profit_threshold_usd{0.0};
        double gamma_pnl_widen_mult{1.0};
        double gamma_pnl_tighten_mult{1.0};
    };

    ASPricer() = default;
    explicit ASPricer(Config cfg) : cfg_(std::move(cfg)) {}
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

    template <class InstrumentState>
    [[nodiscard]] double gamma_pnl_mult(const InstrumentState& st) const {
        if (cfg_.gamma_pnl_window_n == 0 || st.recent_rpnl.empty())
            return 1.0;
        double sum = 0.0;
        for (double r : st.recent_rpnl)
            sum += r;
        if (sum < cfg_.gamma_pnl_loss_threshold_usd)
            return cfg_.gamma_pnl_widen_mult;
        if (sum > cfg_.gamma_pnl_profit_threshold_usd)
            return cfg_.gamma_pnl_tighten_mult;
        return 1.0;
    }

    template <class InstrumentState>
    [[nodiscard]] std::optional<QuoteTarget> evaluate(
        const InstrumentState& st, const BboContext& ctx, double fee_half_spread) const {
        if (st.ewma_var.count() < cfg_.vol_warmup_ticks)
            return std::nullopt;
        if (st.ewma_var.value() <= 0.0)
            return std::nullopt;

        const double mid = ctx.mid;
        const double T_minus_t = std::max(
            0.0, cfg_.session_duration_s - static_cast<double>(ctx.ts_ns - st.session_start_ns) * 1e-9);

        const double sigma_sq = st.ewma_var.value();
        const double effective_gamma = cfg_.gamma * st.regime.gamma_multiplier() * gamma_pnl_mult(st);
        const double gamma_sigma_sq_T = effective_gamma * sigma_sq * T_minus_t;

        // Cartea-Jaimungal drift-adjusted reservation. σ² and µ are log-returns; multiply by mid
        // for price units. q normalized to [-1,1] via max_inventory so γ is scale-invariant
        // (b684b17: unnormalized on cheap instrument blew APE bid to -$2.74 vs $0.16 mid).
        const double q_norm =
            (cfg_.max_inventory > 0.0) ? std::clamp(ctx.net_qty / cfg_.max_inventory, -1.0, 1.0) : 0.0;
        const double inventory_skew_frac = q_norm * gamma_sigma_sq_T;

        // ewma_drift is per-√s (log_ret/√dt); cumulative over T gives µ·√T, not µ·T.
        // µ·T overflows on slow-vol instruments — HL APE T=3600s gave drift_skew_frac=-2.52 pre-fix.
        // Suppressed during warmup — early EWMA values too noisy to project.
        double drift_skew_frac = (st.ewma_drift.count() >= cfg_.drift_warmup_ticks)
                                     ? st.ewma_drift.value() * std::sqrt(T_minus_t)
                                     : 0.0;
        // Cap magnitude — √(T-t) amplification at session start pushes quotes off-book without it.
        if (cfg_.max_drift_skew_bps > 0.0) {
            const double cap = cfg_.max_drift_skew_bps / 10000.0;
            drift_skew_frac = std::clamp(drift_skew_frac, -cap, cap);
        }

        const double ofi_skew_frac = cfg_.ofi_weight_bps * 1e-4 * st.ofi.value();
        double book_imbalance_skew_frac = 0.0;
        if (cfg_.imbalance_weight_bps != 0.0) {
            const double bq = st.book.best_bid_qty();
            const double aq = st.book.best_ask_qty();
            const double denom = bq + aq;
            if (denom > 0.0)
                book_imbalance_skew_frac = cfg_.imbalance_weight_bps * 1e-4 * (bq - aq) / denom;
        }

        const double reservation =
            mid * (1.0 + drift_skew_frac + ofi_skew_frac + book_imbalance_skew_frac - inventory_skew_frac);
        const double drift_adjustment = drift_skew_frac * mid;

        const double kappa = (st.ewma_kappa.count() >= cfg_.kappa_warmup_ticks)
                                 ? std::max(cfg_.kappa_min, st.ewma_kappa.value())
                                 : cfg_.kappa;

        // fee_half_spread = one maker leg; ensures each half-spread covers commissions.
        const double min_half_spread = std::max((cfg_.min_half_spread_bps / 10000.0) * mid, fee_half_spread);
        const double raw_half_spread =
            std::max(min_half_spread,
                     gamma_sigma_sq_T / 2.0 + (1.0 / effective_gamma) * std::log(1.0 + effective_gamma / kappa));

        // Warmup/spike safety clamp; fires when σ² blows up or κ → 0. Rate-limited WARN.
        const double max_half_spread = (cfg_.max_half_spread_bps / 10000.0) * mid;
        double half_spread = raw_half_spread;
        if (raw_half_spread > max_half_spread) {
            half_spread = max_half_spread;
            static std::size_t clamp_count = 0;
            if (++clamp_count <= 5 || clamp_count % 1000 == 0) {
                static quill::Logger* log = bpt::common::logging::get_logger("AS");
                bpt::common::log::warn(
                    log,
                    "half-spread clamp: formula={:.2f} bps → clamped to {:.2f} bps "
                    "(σ²={:.2e} κ={:.4f} ticks={} {}; {} clamps so far)",
                    raw_half_spread / mid * 10000,
                    cfg_.max_half_spread_bps,
                    sigma_sq,
                    kappa,
                    st.ewma_var.count(),
                    (st.ewma_var.count() < cfg_.vol_warmup_ticks * 3) ? "WARMUP" : "σ-SPIKE",
                    clamp_count);
            }
        }

        double bid = reservation - half_spread;
        double ask = reservation + half_spread;

        // Clamp to inside BBO — inventory skew can push reservation through the touch (POST_ONLY
        // reject or taker fill). Return nullopt on crossed market (transient feed state).
        if (st.tick_size > 0.0 && st.last_market_bid > 0.0 && st.last_market_ask > 0.0) {
            if (bid > st.last_market_ask - st.tick_size)
                bid = st.last_market_ask - st.tick_size;
            if (ask < st.last_market_bid + st.tick_size)
                ask = st.last_market_bid + st.tick_size;
            if (bid >= ask)
                return std::nullopt;
        }

        // b684b17: cheap-instrument formula overflow — gate here is cheaper than OM rejecting 900 orders.
        if (st.last_mid > 0.0 && cfg_.quote_sanity_bps > 0.0) {
            const double bound = st.last_mid * (cfg_.quote_sanity_bps / 10000.0);
            if (bid < st.last_mid - bound || ask > st.last_mid + bound || bid <= 0.0) {
                static std::size_t skip_count = 0;
                if (++skip_count <= 5 || skip_count % 1000 == 0) {
                    static quill::Logger* log = bpt::common::logging::get_logger("AS");
                    bpt::common::log::warn(
                        log,
                        "{} quote out of sanity range — skipping tick: "
                        "bid={:.6f} ask={:.6f} mid={:.6f} reservation={:.6f} "
                        "half_spread={:.6f} (sanity_bps={:.1f}; {} skips so far)",
                        st.symbol,
                        bid,
                        ask,
                        st.last_mid,
                        reservation,
                        half_spread,
                        cfg_.quote_sanity_bps,
                        skip_count);
                }
                return std::nullopt;
            }
        }

        {
            static quill::Logger* log = bpt::common::logging::get_logger("AS");
            bpt::common::log::debug(
                log,
                "quotes σ²={:.2e} µ={:.2e} κ={:.4f} ({}) half_spread={:.4f} reservation={:.2f} drift_adj={:.4f}",
                sigma_sq,
                st.ewma_drift.value(),
                kappa,
                (st.ewma_kappa.count() >= cfg_.kappa_warmup_ticks) ? "live" : "fallback",
                half_spread,
                reservation,
                drift_adjustment);
        }

        return QuoteTarget{bid, ask};
    }

private:
    Config cfg_;
};

}  // namespace bpt::strategy::strategy
