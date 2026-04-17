#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/ofi_calculator.h"
#include "strategy/strategy/volatility_gate.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/OrderSide.h>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

// Standalone Order-Flow-Imbalance strategy.
//
// Consumes L2 order-book updates via on_order_book() and feeds them to
// an OFICalculator per instrument. When the normalized rolling OFI
// value crosses `entry_threshold`, fires an IOC market order in the
// signal direction. Positions are exited on the first of:
//   - opposite signal crosses -exit_threshold
//   - stop/target bps from entry
//   - max_hold_seconds elapsed
// After an exit, a per-instrument cooldown (in ticks) gates re-entry
// to prevent flipping on noise.
//
// Taker-only, single position per instrument, no pyramiding. Fixed USD
// notional sizing. Requires `order_book_depth >= 1` in the strategy
// params so bpt-md-gateway delivers MdOrderBook frames.
class OFIStrategy : public IStrategy {
public:
    OFIStrategy(uint64_t correlation_id,
                const config::StrategyConfig& cfg,
                refdata::RefdataClient& refdata,
                md::MdClient* md,
                order::OrderManager* order_mgr);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;
    void on_order_book(const bpt::messages::MdOrderBook& book) override;
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;
    void on_shutdown_flatten() override;

private:
    enum class Position : uint8_t { FLAT, LONG, SHORT };

    // Post-fill mark-out tracker. Records fill price + time + signed
    // direction and logs the mid move at 1s / 5s / 30s anchors so we
    // can tell whether the OFI signal actually caught a real move or
    // we were adversely selected. Diagnostic only — no trading logic
    // hangs off these values.
    struct MarkOut {
        double fill_price{0.0};
        uint64_t fill_ns{0};
        int side_sign{0};        // +1 for BUY/LONG, -1 for SELL/SHORT
        uint64_t order_id{0};
        // True when this fill opened a position, false when it closed one
        // (stop/target/signal-flip/time-stop). Without this flag all exit
        // fills get labelled by BUY/SELL side alone, so a stop-cover of a
        // losing short pollutes the "LONG" markout bucket — the post-fill
        // numbers then blame the wrong side for the loss.
        bool is_entry{true};
        bool logged_1s{false};
        bool logged_5s{false};
        bool logged_30s{false};
    };

    struct InstrumentState {
        uint64_t instrument_id{0};
        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
        double tick_size{0.0};
        double lot_size{0.0};

        // BBO
        double bid{0.0};
        double ask{0.0};
        uint64_t last_bbo_ns{0};

        OFICalculator ofi;

        // Position
        Position pos{Position::FLAT};
        double entry_price{0.0};
        uint64_t entry_ns{0};
        uint64_t active_order_id{0};  // 0 when no order in flight
        bool active_is_entry{false};  // true if active_order_id opens, false if it closes

        // Cooldown after exit (in book ticks)
        int cooldown_ticks_remaining{0};

        // Pending mark-out diagnostics — pushed on every fill, popped
        // once the 30s anchor has been logged. Capped at a dozen entries
        // so a stuck tick stream can't grow this unbounded.
        std::deque<MarkOut> pending_markouts;

        // Per-instrument volatility gate: trips on a short-horizon
        // realized vol spike and blocks new entries until cooldown.
        // Config copied from the strategy-level params at snapshot time.
        VolatilityGate vol_gate;

        explicit InstrumentState(OFICalculator::Config ofi_cfg, VolatilityGate::Config vol_cfg)
            : ofi(ofi_cfg), vol_gate(vol_cfg) {}
    };

    void try_enter(InstrumentState& st, double ofi_value, uint64_t now_ns);
    void try_exit(InstrumentState& st, double ofi_value, uint64_t now_ns);
    void fire_order(InstrumentState& st, bpt::messages::OrderSide::Value side, double qty_usd);

    // Walk pending_markouts and emit a log line for any anchor that
    // has been crossed. Called from on_bbo after bid/ask are refreshed.
    void check_markouts(InstrumentState& st, uint64_t now_ns);

    uint64_t correlation_id_;

    // OFI signal config
    int book_levels_;
    uint64_t ofi_window_ns_;
    double entry_threshold_;
    double exit_threshold_;

    // Exit config
    double stop_bps_;
    double target_bps_;
    uint64_t max_hold_ns_;
    int cooldown_ticks_;

    // Execution config
    double qty_usd_;
    double max_spread_bps_;
    uint8_t order_book_depth_;

    // Volatility gate config — applied per-instrument at snapshot time
    VolatilityGate::Config vol_gate_cfg_;

    // Standard fields
    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::RefdataClient& refdata_;
    md::MdClient* md_client_;
    order::OrderManager* order_mgr_;
    std::unordered_map<uint64_t, InstrumentState> state_;
    std::unordered_map<uint64_t, uint64_t> order_to_instrument_;
};

}  // namespace bpt::strategy::strategy
