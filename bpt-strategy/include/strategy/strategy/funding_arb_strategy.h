#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/i_order_gateway_client.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/position_tracker.h"

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

// Funding rate arbitrage strategy.
//
// For each configured base asset (e.g. BTC), maintains a paired position:
//   - When perp funding rate is positive (longs pay shorts):
//     BUY spot + SELL perp → collect funding on the short perp leg.
//   - When perp funding rate is negative (shorts pay longs):
//     SELL spot + BUY perp → collect funding on the long perp leg.
//
// The spot and perp legs are sized equally so the combined position is
// delta-neutral (market-direction risk hedged).  Profit comes from the
// funding payment, not from price movement.
//
// Latency-insensitive: evaluation runs every eval_interval_s seconds,
// orders are aggressive LIMIT IOC to get immediate fills.
class FundingArbStrategy : public IStrategy {
public:
    FundingArbStrategy(uint64_t correlation_id,
                       const config::StrategyConfig& cfg,
                       refdata::RefdataClient& refdata,
                       md::MdClient* md,
                       order::IOrderGatewayClient* order_gw);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;

private:
    enum class PairState : uint8_t {
        IDLE,                 // No position; evaluating funding rate.
        ENTERING_FIRST_LEG,   // First leg order sent, awaiting fill.
        ENTERING_SECOND_LEG,  // First leg filled, second leg order sent.
        ACTIVE,               // Both legs filled; collecting funding.
        EXITING_FIRST_LEG,    // Exit: first leg order sent.
        EXITING_SECOND_LEG,   // Exit: first leg closed, second leg order sent.
        UNWINDING,            // Failed entry — closing the filled leg to return to IDLE.
    };

    static const char* state_name(PairState s);

    struct LegState {
        uint64_t instrument_id{0};
        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};

        double tick_size{0.0};
        double lot_size{0.0};

        // Latest BBO
        double bid{0.0};
        double ask{0.0};
        uint64_t last_bbo_ns{0};

        // Active order on this leg (0 = none).
        uint64_t order_id{0};
    };

    struct ArbPair {
        std::string base_asset;
        LegState spot;
        LegState perp;

        PairState state{PairState::IDLE};

        // +1 = long spot / short perp, -1 = short spot / long perp, 0 = no position.
        int direction{0};

        // Funding rate monitoring.
        int32_t last_funding_rate_bps{0};
        int stable_sign_count{0};
        int last_funding_sign{0};  // +1, -1, or 0

        // Entry prices for basis PnL tracking.
        double spot_entry_mid{0.0};
        double perp_entry_mid{0.0};

        // Filled quantities (raw 1e5 fixed-point from PositionTracker).
        int64_t spot_filled_qty{0};
        int64_t perp_filled_qty{0};

        uint64_t entry_ts{0};
        uint64_t last_eval_ts{0};
    };

    // Core decision logic — called on BBO updates when both legs have fresh prices.
    void evaluate_pair(ArbPair& pair, uint64_t now_ns);
    void enter_position(ArbPair& pair, int direction, uint64_t now_ns);
    void exit_position(ArbPair& pair, uint64_t now_ns);
    void send_leg_order(ArbPair& pair, LegState& leg, bpt::messages::OrderSide::Value side, uint64_t qty);
    void handle_fill(ArbPair& pair, const LegState& leg, const bpt::messages::ExecutionReport& rpt);
    void handle_terminal(ArbPair& pair, LegState& leg, bpt::messages::ExecStatus::Value status);

    // Compute matched quantity for both legs (respecting lot sizes).
    uint64_t compute_leg_qty(const ArbPair& pair, double mid) const;

    // Config params
    int32_t min_funding_rate_bps_;
    int32_t exit_funding_rate_bps_;
    int min_stable_periods_;
    uint64_t min_time_before_funding_ns_;
    uint64_t eval_interval_ns_;
    double max_basis_loss_bps_;
    double target_notional_usd_;
    uint64_t order_timeout_ns_;
    double aggress_bps_;  // how far through the spread to cross for IOC fills

    // Standard fields
    uint64_t correlation_id_;
    std::vector<std::string> instruments_;
    std::vector<std::string> base_assets_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::RefdataClient& refdata_;
    md::MdClient* md_client_;
    order::IOrderGatewayClient* order_gw_;

    std::atomic<uint64_t> next_order_id_{1};
    std::unordered_map<std::string, ArbPair> pairs_;                // keyed by base asset
    std::unordered_map<uint64_t, std::string> instrument_to_base_;  // instrument_id → base
    std::unordered_map<uint64_t, std::string> order_to_base_;       // order_id → base
    PositionTracker positions_;
};

}  // namespace bpt::strategy::strategy
