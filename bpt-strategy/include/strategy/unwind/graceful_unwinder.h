#pragma once

#include "strategy/order/order_manager.h"
#include "strategy/strategy/position_tracker.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bpt::strategy::unwind {

// Shared graceful-shutdown unwind state machine for MM strategies.
//
// Protocol:
//   1. Strategy cancels resting quotes (cancel_all + per-handle cancels).
//   2. Strategy calls arm() with per-instrument metadata.
//   3. Drain loop calls tick() every iteration, poll()s exec reports.
//   4. Strategy routes any exec report whose tag == kTag to on_exec_report().
//   5. Loop exits when pending() == false (or drain budget expires).
//
// State machine per instrument:
//   kPassive — GTC LIMIT at fair value, step one tick toward market every
//              step_interval_s. Transitions to kIoc after passive_timeout_s.
//   kIoc     — IOC at unwind_price ± cross_bps, retries with +10 bps each time.
//   kDone    — position flat or retries exhausted.
class GracefulUnwinder {
public:
    static constexpr uint8_t kTag = 9;

    struct Config {
        double passive_timeout_s = 45.0;
        double step_interval_s = 8.0;
        double cross_bps = 20.0;
        uint32_t max_retries = 3;
    };

    struct Instrument {
        uint64_t instrument_id;
        bpt::messages::ExchangeId::Value exchange_id;
        double tick_size;
        double lot_size;
        std::string symbol;
        double price_ref;  // FV or last_mid at arm() time; IOC derived from stepped price
    };

    GracefulUnwinder(strategy::PositionTracker& positions, order::OrderManager& order_mgr, Config cfg);

    void arm(std::vector<Instrument> instruments);
    void tick();
    void on_exec_report(const bpt::messages::ExecutionReport& rpt);
    [[nodiscard]] bool pending() const;
    [[nodiscard]] double drain_budget_s() const;

private:
    enum class Phase : uint8_t { kPassive, kIoc, kDone };

    struct Record {
        uint64_t instrument_id;
        bpt::messages::ExchangeId::Value exchange_id;
        double tick_size;
        double lot_size;
        std::string symbol;
        Phase phase{Phase::kPassive};
        uint64_t start_ns{0};
        uint64_t last_step_ns{0};
        double unwind_price{0.0};
        order::OrderHandle h_unwind;
        uint32_t retries_left{0};
        uint32_t attempt{0};
    };

    void send_passive(Record& r, bpt::messages::OrderSide::Value side, double qty);
    void send_ioc(Record& r, bpt::messages::OrderSide::Value side, double qty, double cross_bps);
    void advance_ioc(Record& r);
    [[nodiscard]] std::pair<double, double> net_and_abs(const Record& r) const;

    strategy::PositionTracker& positions_;
    order::OrderManager& order_mgr_;
    Config cfg_;
    std::vector<Record> records_;
};

}  // namespace bpt::strategy::unwind
