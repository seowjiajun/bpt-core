#pragma once

/// @file
/// Fair-value estimators for AS-style market making.
///
/// AS uses a reference price `s` in its reservation-price formula:
///     r = s + drift - q * γ * σ² * (T - t)
/// The choice of `s` matters: mid is biased on imbalanced books, micro
/// is closer to a true martingale (Stoikov 2017). Different markets and
/// strategy variants may want different estimators, and we want to A/B
/// them in backtest and prod. This class is the single-point abstraction.
///
/// Design choices:
///   - Enum-tagged switch, NOT virtual dispatch — hot-path call, want
///     it inlined into the strategy loop. CPU branch predictor handles
///     the switch perfectly because mode is fixed at construction.
///   - Stateful (EWMA needs to remember the previous value), so it's
///     a class rather than free functions.
///   - Per-instrument: each InstrumentState owns one. Per-instrument
///     EWMA state is what you want.
///   - Returns NaN if the book has no top-of-book on either side. The
///     caller (strategy) already gates on book.ready(); we re-check
///     defensively rather than UB.

#include "features/order_book_state.h"

#include <cmath>
#include <cstddef>
#include <limits>

namespace bpt::features {

class FairValueEstimator {
public:
    enum class Mode {
        /// (bid + ask) / 2. Naive baseline, useful as A/B reference.
        kMid,

        /// (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty).
        /// Tilts toward the side with less liquidity — that side is
        /// the one about to move first. Stoikov's micro-price.
        kMicro,

        /// Same as kMicro but each side's qty is clamped at
        /// Config::size_cap_qty. Defends against iceberg / displayed-
        /// size manipulation where one side momentarily shows a fat
        /// queue larger than its true intent.
        kMicroSizeCapped,

        /// L2-weighted micro: top-of-book prices, but qty is the sum
        /// of the first Config::ladder_depth levels with exponential
        /// decay (Config::ladder_decay^level). Smoother on thin TOB.
        kL2WeightedMicro,

        /// EWMA-smoothed micro: alpha * micro + (1-alpha) * previous.
        /// Useful when raw micro is too jittery for your re-quote
        /// cadence. Adds one tick of lag.
        kEwmaMicro,
    };

    struct Config {
        Mode mode = Mode::kMid;

        /// kMicroSizeCapped: per-side qty clamp. 0 = no cap (acts like kMicro).
        double size_cap_qty = 0.0;

        /// kL2WeightedMicro: number of ladder levels to weight in.
        std::size_t ladder_depth = 5;

        /// kL2WeightedMicro: weight at level i is `decay^i` (level 0 = top).
        /// 1.0 = uniform; 0.5 = half-weight per step deeper. Must be in (0, 1].
        double ladder_decay = 0.5;

        /// kEwmaMicro: smoothing factor. Higher = more responsive, less smooth.
        /// Must be in (0, 1].
        double ewma_alpha = 0.3;
    };

    /// Default constructor uses Mode::kMid + defaults — safe for use as
    /// a member of an aggregate before the strategy supplies its own config.
    FairValueEstimator() noexcept = default;
    explicit FairValueEstimator(Config cfg) noexcept : cfg_(cfg) {}

    /// Compute the fair-value estimate for the current book state.
    ///
    /// Returns NaN if the book has fewer than one level on either side.
    /// Updates internal state (last value cache, EWMA history).
    [[nodiscard]] double estimate(const OrderBookState& book) noexcept {
        if (!book.ready()) {
            last_ = std::numeric_limits<double>::quiet_NaN();
            return last_;
        }
        if (cfg_.mode == Mode::kL2WeightedMicro) {
            last_ = l2_weighted(book, cfg_.ladder_depth, cfg_.ladder_decay);
            return last_;
        }
        // All other modes only need top-of-book.
        return estimate(book.best_bid(), book.best_ask(), book.best_bid_qty(), book.best_ask_qty());
    }

    /// TOB-only overload for hot-path use (BBO ticks have no L2 ladder).
    ///
    /// Mode::kL2WeightedMicro silently degrades to plain micro on this
    /// path — the L2 ladder isn't available, so there's nothing to
    /// weight. Callers wanting genuine L2 weighting must use the
    /// OrderBookState overload.
    ///
    /// Returns NaN on a degenerate quote (non-positive prices, crossed,
    /// or zero total qty); state is updated to NaN as well.
    [[nodiscard]] double estimate(double bid_px, double ask_px, double bid_qty, double ask_qty) noexcept {
        if (bid_px <= 0.0 || ask_px <= 0.0 || ask_px <= bid_px) {
            last_ = std::numeric_limits<double>::quiet_NaN();
            return last_;
        }
        switch (cfg_.mode) {
            case Mode::kMid:
                last_ = 0.5 * (bid_px + ask_px);
                break;
            case Mode::kMicro:
            case Mode::kL2WeightedMicro:  // degraded — no L2 here
                last_ = micro_tob(bid_px, ask_px, bid_qty, ask_qty);
                break;
            case Mode::kMicroSizeCapped:
                last_ = micro_capped_tob(bid_px, ask_px, bid_qty, ask_qty, cfg_.size_cap_qty);
                break;
            case Mode::kEwmaMicro:
                last_ = update_ewma(micro_tob(bid_px, ask_px, bid_qty, ask_qty));
                break;
        }
        return last_;
    }

    /// Last computed value, no recomputation. Returns NaN if estimate()
    /// has never been called or the most recent call saw an unready book.
    [[nodiscard]] double last_estimate() const noexcept { return last_; }

    [[nodiscard]] Mode mode() const noexcept { return cfg_.mode; }

private:
    Config cfg_;
    double last_ = std::numeric_limits<double>::quiet_NaN();
    double ewma_state_ = std::numeric_limits<double>::quiet_NaN();

    /// Per-instance scratch reused by the L2-weighted path. The book
    /// lookups call into OrderBookState's buffer-fill overload which
    /// `.clear()`s and refills — zero allocation after the first
    /// reserve grows the buffer.
    mutable std::vector<OrderBookState::Level> l2_bids_scratch_;
    mutable std::vector<OrderBookState::Level> l2_asks_scratch_;

    static double micro_tob(double bid_px, double ask_px, double bid_qty, double ask_qty) noexcept {
        const double total = bid_qty + ask_qty;
        if (total <= 0.0)
            return 0.5 * (bid_px + ask_px);  // degenerate qty — fall back to mid
        return (bid_px * ask_qty + ask_px * bid_qty) / total;
    }

    static double micro_capped_tob(double bid_px, double ask_px, double bid_qty, double ask_qty, double cap) noexcept {
        const double bq = (cap > 0.0) ? std::fmin(bid_qty, cap) : bid_qty;
        const double aq = (cap > 0.0) ? std::fmin(ask_qty, cap) : ask_qty;
        return micro_tob(bid_px, ask_px, bq, aq);
    }

    double l2_weighted(const OrderBookState& b, std::size_t depth, double decay) const noexcept {
        b.top_bids(depth, l2_bids_scratch_);
        b.top_asks(depth, l2_asks_scratch_);
        const double bp = l2_bids_scratch_.front().price;
        const double ap = l2_asks_scratch_.front().price;

        double bq_w = 0.0, aq_w = 0.0;
        double w = 1.0;
        for (const auto& lvl : l2_bids_scratch_) {
            bq_w += lvl.qty * w;
            w *= decay;
        }
        w = 1.0;
        for (const auto& lvl : l2_asks_scratch_) {
            aq_w += lvl.qty * w;
            w *= decay;
        }

        const double total = bq_w + aq_w;
        if (total <= 0.0)
            return 0.5 * (bp + ap);
        return (bp * aq_w + ap * bq_w) / total;
    }

    double update_ewma(double x) noexcept {
        if (std::isnan(ewma_state_))
            ewma_state_ = x;
        else
            ewma_state_ = cfg_.ewma_alpha * x + (1.0 - cfg_.ewma_alpha) * ewma_state_;
        return ewma_state_;
    }
};

}  // namespace bpt::features
