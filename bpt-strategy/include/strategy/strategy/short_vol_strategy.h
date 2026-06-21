#pragma once

#include "features/realized_vol.h"
#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/position_tracker.h"
#include "strategy/vol/vol_surface_client.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/OptionSide.h>
#include <messages/VolSurface.h>

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

using bpt::features::RealizedVolEstimator;

// Delta-neutral short volatility strategy.
//
// Signal: sell options where IV > RV by a configurable threshold.
// Hedge: delta-hedge each option position with perp on the same underlying.
//
// Data flow:
//   - Reads VolSurface from Pricer (stream 4001) for IV, Greeks, option BBO.
//   - Reads perp BBO from MdGateway (stream 2002) for delta-hedge execution prices.
//   - Sends option + perp orders to OrderGateway (stream 3001).
//
// Single-threaded: all callbacks called from the main poll loop.
class ShortVolStrategy : public IStrategy {
public:
    ShortVolStrategy(uint64_t correlation_id,
                     const config::StrategyConfig& cfg,
                     refdata::IRefdataClient& refdata,
                     md::IMdClient* md,
                     order::OrderManager* order_mgr,
                     vol::IVolSurfaceClient* vol_client);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;
    void on_vol_surface(bpt::messages::VolSurface& surface) override;
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;
    PortfolioState get_portfolio_state() override;

private:
    // Per-option position tracking.
    struct OptionLeg {
        uint64_t instrument_id{0};
        uint32_t expiry_date{0};
        double strike{0.0};
        bool is_call{true};

        // From latest VolSurface
        double iv{0.0};
        double bid_price{0.0};
        double ask_price{0.0};
        double delta{0.0};
        double gamma{0.0};
        double vega{0.0};
        double theta{0.0};
        double forward_price{0.0};
        double time_to_expiry{0.0};

        // Position
        double position_qty{0.0};  // negative = short
        double entry_price{0.0};

        // Active order
        uint64_t order_id{0};
    };

    // Per-underlying state.
    struct UnderlyingState {
        std::string underlying;
        bpt::messages::ExchangeId::Value exchange_id{};

        // Perp hedge leg
        uint64_t perp_instrument_id{0};
        double perp_bid{0.0};
        double perp_ask{0.0};
        uint64_t perp_last_bbo_ns{0};
        double perp_position_qty{0.0};
        uint64_t perp_order_id{0};
        double perp_tick_size{0.0};
        double perp_lot_size{0.0};

        // Realized vol estimator (fed from perp BBO mid)
        RealizedVolEstimator rv_estimator;

        // Option legs keyed by instrument_id
        std::unordered_map<uint64_t, OptionLeg> options;

        // Portfolio Greeks (sum across all option positions)
        double portfolio_delta{0.0};
        double portfolio_gamma{0.0};
        double portfolio_vega{0.0};
        double portfolio_theta{0.0};

        // Last evaluation timestamp
        uint64_t last_eval_ns{0};

        UnderlyingState() : rv_estimator(0, 1) {}

        UnderlyingState(size_t rv_window, uint64_t rv_interval_ns) : rv_estimator(rv_window, rv_interval_ns) {}
    };

    // Evaluate whether to open/close option positions for this underlying.
    void evaluate(UnderlyingState& state, uint64_t now_ns);

    // Rebalance the perp hedge to match portfolio delta.
    void rebalance_hedge(UnderlyingState& state, uint64_t now_ns);

    // Send an option order (sell to open, buy to close).
    void send_option_order(UnderlyingState& state,
                           OptionLeg& leg,
                           bpt::messages::OrderSide::Value side,
                           double qty,
                           double price);

    // Send a perp hedge order.
    void send_perp_order(UnderlyingState& state, bpt::messages::OrderSide::Value side, double qty, double price);

    // Recompute portfolio Greeks for an underlying.
    void recompute_greeks(UnderlyingState& state);

    // Config params
    double iv_rv_threshold_;          // minimum IV - RV spread to enter (e.g. 0.05 = 5 vol points)
    double iv_rv_exit_threshold_;     // close position when IV - RV narrows below this
    double max_portfolio_delta_;      // max abs portfolio delta before forced rebalance
    double max_portfolio_vega_;       // max abs portfolio vega (risk limit)
    double max_portfolio_gamma_;      // max abs portfolio gamma (risk limit)
    double target_notional_usd_;      // target notional per option entry
    double min_time_to_expiry_;       // don't sell options expiring sooner than this (years)
    double max_time_to_expiry_;       // don't sell options expiring later than this (years)
    double min_abs_delta_;            // filter: only sell options with |delta| in range
    double max_abs_delta_;            // (e.g. 0.1 to 0.5 — avoid deep ITM/OTM)
    uint64_t eval_interval_ns_;       // how often to evaluate
    double aggress_bps_;              // how far through spread to cross for IOC
    size_t rv_window_;                // realized vol window size
    uint64_t rv_sample_interval_ns_;  // realized vol sample interval

    // Standard fields
    uint64_t correlation_id_;
    std::vector<std::string> instruments_;
    std::vector<std::string> underlyings_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;
    config::RiskConfig risk_;

    refdata::IRefdataClient& refdata_;
    md::IMdClient* md_client_;
    order::OrderManager* order_mgr_;
    vol::IVolSurfaceClient* vol_client_;

    // Keyed by "exchange_id:underlying" (same key as Pricer's grid)
    std::unordered_map<std::string, UnderlyingState> states_;

    // Reverse lookups
    std::unordered_map<uint64_t, std::string> instrument_to_key_;  // instrument_id → state key
    std::unordered_map<uint64_t, std::string> order_to_key_;       // order_id → state key
    std::unordered_map<uint64_t, bool> order_is_perp_;             // order_id → true if perp order

    PositionTracker positions_;

    static std::string state_key(bpt::messages::ExchangeId::Value ex, const std::string& underlying);
};

}  // namespace bpt::strategy::strategy
