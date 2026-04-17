#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/i_strategy.h"

#include <messages/ExchangeId.h>
#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>

#include <atomic>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

// Price-momentum strategy.
//
// For each subscribed instrument, maintains a rolling window of mid-prices
// (bid+ask)/2.  Once the window is full, computes:
//
//   momentum = (current_mid - oldest_mid) / oldest_mid
//
// If momentum > +entry_threshold  → log BUY signal
// If momentum < -entry_threshold  → log SELL signal
//
// Orders are NOT sent (order gateway not yet implemented).
// A per-instrument cooldown_ms prevents back-to-back signals.
class MomentumStrategy : public IStrategy {
public:
    MomentumStrategy(uint64_t correlation_id,
                     const config::StrategyConfig& cfg,
                     refdata::RefdataClient& refdata,
                     md::MdClient* md,
                     order::OrderManager* order_mgr);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;

private:
    struct InstrumentState {
        std::deque<double> prices;  // rolling mid-price window
        uint64_t last_signal_ns{0};
        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
    };

    void emit_signal(uint64_t instrument_id,
                     InstrumentState& state,
                     double current_mid,
                     double momentum,
                     uint64_t timestamp_ns);

    uint64_t correlation_id_;
    std::size_t lookback_;                  // number of ticks in the window
    double entry_threshold_;                // fractional momentum required to signal
    uint64_t cooldown_ns_;                  // min nanoseconds between signals per instrument
    std::vector<std::string> instruments_;  // canonical symbols: BASE/QUOTE:TYPE
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::RefdataClient& refdata_;
    md::MdClient* md_client_;
    order::OrderManager* order_mgr_;
    std::unordered_map<uint64_t, InstrumentState> state_;  // keyed by instrument_id
};

}  // namespace bpt::strategy::strategy
