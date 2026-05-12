#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/realized_vol_estimator.h"
#include "strategy/strategy/regime_classifier.h"
#include "strategy/strategy/volatility_gate.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/OrderSide.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

// PassiveMakerStrategy — wide-spread, low-cadence market maker.
//
// Different from AvellanedaStoikovStrategy in three ways:
//   (1) The half-spread is a FIXED parameter, not derived from γ/κ each tick.
//   (2) Quotes only requote on three triggers: inventory cap, fair-value
//       drift > requote_threshold_bps, or vol gate halt. AS reprices on
//       every book update.
//   (3) Reservation price r = FV − q × c uses a simple linear inventory
//       penalty instead of AS's gamma·sigma²·(T−t). One coefficient, one
//       knob.
//
// Designed for low-trade-frequency markets where AS over-quotes (e.g.
// HL APE has ~275 trades/day; AS reprices ~3000 times/day → orders are
// cancelled before they have a chance to fill). Passive maker holds
// quotes for minutes, accepting more inventory drift in exchange for
// matching the market's actual trade arrival cadence.
//
// Posts as POST_ONLY LIMIT (Add-Liquidity-Only on HL): the strategy
// never crosses the touch and never pays taker fees.
class PassiveMakerStrategy : public IStrategy {
public:
    PassiveMakerStrategy(uint64_t correlation_id,
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
    void on_shutdown_flatten() override;

    [[nodiscard]] bool has_pending_flatten() const override;

    // ── Public static helpers (exposed for unit tests) ──────────────────

    // Compute microprice from L1: (bid×ask_sz + ask×bid_sz) / (bid_sz + ask_sz).
    // Falls back to mid when sizes are zero. Microprice has the well-known
    // property of leaning toward the side with thinner depth, which is the
    // fair direction for an inventory-neutral maker.
    static inline double compute_fair_value(double bid, double ask,
                                             double bid_sz, double ask_sz) {
        if (bid <= 0.0 || ask <= 0.0)
            return 0.0;
        const double total_sz = bid_sz + ask_sz;
        if (total_sz <= 0.0)
            return (bid + ask) * 0.5;
        return (bid * ask_sz + ask * bid_sz) / total_sz;
    }

    // Round to the instrument's tick. POST_ONLY rejects on unaligned price.
    static inline double round_to_tick(double price, double tick_size, bool round_down) {
        if (tick_size <= 0.0)
            return price;
        const double n = price / tick_size;
        return (round_down ? std::floor(n) : std::ceil(n)) * tick_size;
    }

    // Linear scale of a base parameter by a vol-derived term: out = base + mult × vol_bps.
    // Pure function to keep the regime-gating math unit-testable without the
    // RealizedVolEstimator state. Caller plumbs in vol_bps (1-minute realized
    // vol in bps; 0 if estimator not warm).
    static inline double scale_with_vol(double base, double mult, double vol_bps) {
        return base + mult * vol_bps;
    }

    // Convert annualized realized vol (fractional, e.g. 0.50 for 50% APR)
    // to a 1-minute-horizon stdev in bps. Constant 525600 = minutes/year.
    // Used to bring the estimator's annualized output to a quote-relevant
    // horizon before applying the multiplier.
    static inline double annualized_to_per_minute_bps(double annualized_fraction) {
        if (annualized_fraction <= 0.0)
            return 0.0;
        // sqrt(525600) ≈ 725.0; pre-computed at compile time.
        constexpr double kSqrtMinutesPerYear = 725.0327440949459;
        return annualized_fraction * 1e4 / kSqrtMinutesPerYear;
    }

private:
    struct InstrumentState {
        uint64_t instrument_id{0};
        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
        double tick_size{0.0};
        double lot_size{0.0};

        // Latest top-of-book + microprice
        double bid{0.0};
        double ask{0.0};
        double bid_size{0.0};
        double ask_size{0.0};
        uint64_t last_book_ns{0};

        // Net inventory in instrument units. Positive = long, negative = short.
        double inventory{0.0};

        // Active resting orders. 0 = no order on that side.
        uint64_t bid_order_id{0};
        uint64_t ask_order_id{0};
        // Prices of the active resting orders so we can recompute drift
        // from the time we placed, not from the most recent quote we
        // would have placed.
        double bid_order_price{0.0};
        double ask_order_price{0.0};
        // Fair value at the moment we placed the resting orders. Used
        // for the requote_threshold trigger: only requote when |FV_now −
        // FV_at_place| > requote_threshold_bps × FV_at_place / 1e4.
        double fv_at_place{0.0};

        VolatilityGate vol_gate;

        // Realized-vol estimator drives the legacy linear-vol-scaling path
        // (kept for back-compat when regime_classifier is disabled).
        RealizedVolEstimator vol_est;

        // RegimeClassifier — primary regime gate. When enabled (via the
        // regime_classifier_enabled flag), CHOPPY → strategy pauses;
        // TRENDING / QUIET → strategy quotes at base params.
        RegimeClassifier regime;

        // Set during shutdown flatten to suppress new quotes.
        bool flatten_in_progress{false};

        // Per-active-order cumulative filledQty seen so far. Lets us
        // compute the *delta* on each PARTIAL/FILLED exec report so the
        // inventory accumulator works correctly across multiple fills
        // on a single order.
        std::unordered_map<uint64_t, uint64_t> last_filled_qty_e8;

        InstrumentState(VolatilityGate::Config vol_cfg,
                        std::size_t rv_window, std::uint64_t rv_interval_ns,
                        RegimeClassifier::Config regime_cfg)
            : vol_gate(vol_cfg), vol_est(rv_window, rv_interval_ns),
              regime(regime_cfg) {}
    };

    // Cancel any active orders, then post fresh bid/ask using the new
    // reservation price. Sets fv_at_place. Caller checks vol gate.
    void requote(InstrumentState& st, double fv, uint64_t now_ns);

    // True if FV has moved more than requote_threshold_bps from where
    // we placed the active orders.
    bool drift_exceeds_threshold(const InstrumentState& st, double fv_now) const;

    // Place a single side (bid or ask). Returns 0 on rejection.
    uint64_t place_side(InstrumentState& st,
                        bpt::messages::OrderSide::Value side,
                        double price);

    uint64_t correlation_id_;

    // Strategy params
    double half_spread_bps_;            // H — base half-spread (vol scales on top)
    double inventory_penalty_;          // c — price units per inventory unit
    double requote_threshold_bps_;      // base requote threshold (vol scales on top)
    double qty_per_quote_;              // size on each side, in instrument units
    double max_inventory_;              // hard cap; pause one side past this

    // Regime-gating params. Both default to 0.0 (no scaling), so a vanilla
    // config falls back to the fixed-param baseline. Real deployment uses
    // mult ≈ 1.0 to widen quotes proportional to recent realized vol.
    double spread_vol_mult_;            // bps added to half-spread per bps/min vol
    double requote_vol_mult_;           // bps added to threshold per bps/min vol

    // RealizedVolEstimator window. window_size × sample_interval should
    // span a regime-relevant horizon — minutes, not seconds, so the
    // estimator filters out single-tick noise.
    std::size_t vol_window_size_;
    std::uint64_t vol_sample_interval_ns_;

    // Regime classifier — when enabled, gates new quotes on CHOPPY regime.
    bool regime_gating_enabled_;
    RegimeClassifier::Config regime_cfg_;

    VolatilityGate::Config vol_gate_cfg_;

    // Standard fields
    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::IRefdataClient& refdata_;
    md::IMdClient* md_client_;
    order::OrderManager* order_mgr_;

    std::unordered_map<uint64_t, InstrumentState> state_;
    // Map order_id → instrument_id for fast exec-report dispatch.
    std::unordered_map<uint64_t, uint64_t> order_to_instrument_;
};

}  // namespace bpt::strategy::strategy
