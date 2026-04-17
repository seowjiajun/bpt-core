#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
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

// Mid-EMA reversion strategy (trades-free variant of VWAP reversion).
//
// Per instrument:
//   1. Maintain an EMA of mid-price from BBO ticks (period = vwap_window_trades,
//      alpha = 2 / (period + 1)).
//   2. On each BBO tick compute:
//        deviation = (mid - ema) / ema
//   3. Entry (no open position, no open order, cooldown elapsed, min BBO ticks seen):
//        deviation > +entry_threshold  → SELL  (price stretched above EMA; expect reversion)
//        deviation < -entry_threshold  → BUY   (price stretched below EMA; expect reversion)
//   4. Exit (have open position, no open order):
//        Long  → SELL when mid ≥ ema × (1 - exit_threshold)   [reverted]
//             OR SELL when mid ≤ avg_entry × (1 - stop_threshold) [stop loss]
//        Short → BUY  when mid ≤ ema × (1 + exit_threshold)   [reverted]
//             OR BUY  when mid ≥ avg_entry × (1 + stop_threshold) [stop loss]
//
// Config params reused without rename:
//   vwap_window_trades  → EMA period N  (alpha = 2/(N+1))
//   min_trades_to_signal → minimum BBO ticks before signalling
class VwapReversionStrategy : public IStrategy {
public:
    VwapReversionStrategy(uint64_t correlation_id,
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

private:
    struct InstrumentState {
        // EMA of mid-price (computed from BBO ticks)
        double ema_mid{0.0};
        std::size_t bbo_count{0};  // ticks seen so far (warm-up guard)

        // One open order per instrument at a time (0 = none)
        uint64_t open_order_id{0};
        bpt::messages::OrderSide::Value open_order_side{bpt::messages::OrderSide::NULL_VALUE};

        // Cooldown — suppress new entry signals for cooldown_ns_ after the last one
        uint64_t last_signal_ns{0};

        // Instrument metadata
        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
    };

    void send_order(uint64_t instrument_id,
                    InstrumentState& st,
                    bpt::messages::OrderSide::Value side,
                    double mid,
                    double vwap,
                    double quantity,
                    uint64_t timestamp_ns,
                    const char* reason);

    uint64_t correlation_id_;

    // Config params
    std::size_t ema_period_;     // vwap_window_trades in config → EMA period
    std::size_t min_bbo_ticks_;  // min_trades_to_signal in config → warm-up guard
    double ema_alpha_;           // 2 / (ema_period_ + 1)
    double entry_threshold_;
    double exit_threshold_;
    double stop_threshold_;
    uint64_t cooldown_ns_;

    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::RefdataClient& refdata_;
    md::MdClient* md_client_;
    order::OrderManager* order_mgr_;
    std::unordered_map<uint64_t, InstrumentState> state_;  // keyed by instrument_id
    PositionTracker positions_;
    std::unordered_map<uint64_t, uint64_t> order_to_instrument_;  // order_id → instrument_id
};

}  // namespace bpt::strategy::strategy
