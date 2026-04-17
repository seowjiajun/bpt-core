#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/position_tracker.h"
#include "strategy/strategy/regime_detector.h"
#include "strategy/strategy/volatility_gate.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/OrderSide.h>

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

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
                              refdata::RefdataClient& refdata,
                              md::MdClient* md,
                              order::OrderManager* order_mgr);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;
    void on_toxicity_update(const bpt::analytics::messaging::ToxicityUpdate& update) override;
    std::string get_strategy_state_json() override;
    void on_shutdown_flatten() override;

private:
    struct InstrumentState {
        // EWMA volatility state.
        // σ²_t = λ_t * σ²_{t-1} + (1 - λ_t) * norm_ret²
        // λ_t  = exp(-dt_s / vol_halflife_s)  — recomputed per tick for time consistency.
        double ewma_var{0.0};       // current EWMA per-second variance σ²
        std::size_t ewma_ticks{0};  // BBO ticks seen (warmup guard)
        double last_mid{0.0};
        uint64_t last_tick_ns{0};

        // EWMA drift (µ) — signed normalized return, same EWMA machinery as σ².
        // µ_t = λ_t * µ_{t-1} + (1 - λ_t) * norm_ret
        // Positive = uptrend, negative = downtrend. Used to shift the
        // reservation price and suppress the adverse side in momentum regimes.
        double ewma_drift{0.0};

        // EWMA market-order arrival rate κ (trades per second, per side).
        // Estimated from inter-trade intervals on the public trade feed.
        // κ_t = λ_t * κ_{t-1} + (1 - λ_t) * (0.5 / dt_s)
        // The 0.5 factor splits the total trade rate across bid and ask sides.
        double ewma_kappa{0.0};      // current EWMA κ estimate (trades/s per side)
        std::size_t kappa_ticks{0};  // trade ticks seen (warmup guard)
        uint64_t last_trade_ns{0};   // timestamp of last trade (ns)

        // Two-sided quoting: one resting order per side (0 = no live order).
        uint64_t bid_order_id{0};
        uint64_t ask_order_id{0};

        // Set when a cancel request has been sent but the CANCELLED confirm not yet received.
        // While pending, do not send a new order on that side.
        bool bid_cancel_pending{false};
        bool ask_cancel_pending{false};

        // LIMIT IOC order to unwind inventory when it hits max_inventory_.
        // Non-zero while an unwind order is live (waiting for terminal status).
        uint64_t unwind_order_id{0};

        // Prices of the currently live (or most recently placed) orders.
        // Used to detect whether the quote has drifted beyond requote_threshold_.
        double last_bid_price{0.0};
        double last_ask_price{0.0};

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
        double tyr_bid_toxicity{0.0};
        double tyr_ask_toxicity{0.0};
        bool tyr_data_received{false};
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

    // Place a LIMIT IOC order at an aggressive price to unwind inventory.
    // Returns the assigned order_id (0 on failure).
    uint64_t send_unwind_order(uint64_t instrument_id,
                               InstrumentState& st,
                               bpt::messages::OrderSide::Value side,
                               double mid,
                               double qty);

    // Place a LIMIT GTC order and return the assigned order_id (0 on failure).
    uint64_t send_limit_order(uint64_t instrument_id,
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
    double max_inventory_;            // max |net position| in base units
    double order_qty_;                // quote size in natural units (e.g. 0.001 BTC)
    double min_half_spread_bps_;      // floor on half-spread expressed in basis points

    // Drift (momentum) detection — Cartea-Jaimungal extension of AS.
    double drift_halflife_s_;         // EWMA half-life for µ estimation (seconds)
    double drift_suppress_bps_;       // suppress adverse side when |µ| > this (bps/√s)

    // Analytics toxicity suppression — suppress side when tyr score < threshold.
    // 0 disables. Typical value: -2.0 (suppress when 5s markout is -2bps or worse).
    double tyr_suppress_threshold_;   // negative value; 0 disables

    // Regime detector config — applied per-instrument at snapshot time.
    RegimeDetector::Config regime_cfg_;

    // Volatility gate config — applied per-instrument at snapshot time.
    VolatilityGate::Config vol_gate_cfg_;

    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::RefdataClient& refdata_;
    md::MdClient* md_client_;
    order::OrderManager* order_mgr_;
    std::unordered_map<uint64_t, InstrumentState> state_;         // keyed by instrument_id
    std::unordered_map<uint64_t, uint64_t> order_to_instrument_;  // order_id → instrument_id
    PositionTracker positions_;
};

}  // namespace bpt::strategy::strategy
