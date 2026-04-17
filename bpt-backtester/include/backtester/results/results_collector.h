#pragma once

#include "backtester/data/market_event.h"
#include "backtester/matching/open_order.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::backtester::results {

// Collects fills and market prices throughout a backtest run and writes
// three output files when write() is called:
//
//   {output_dir}/trades.csv      — one row per fill
//   {output_dir}/pnl_curve.csv  — equity sampled at every fill
//   {output_dir}/summary.json   — aggregate metrics
//
// Position accounting uses the average-cost method.  Fills that close an
// existing position realise PnL immediately; fills that open or extend a
// position update the running average cost.
class ResultsCollector {
public:
    ResultsCollector(double starting_capital, std::string output_dir);

    // Called on every fill from MatchingEngine.
    void on_fill(const matching::FillReport& fill);

    // Called on every market event from ClockMaster.
    // Used to keep a mark-to-market mid price per symbol for unrealized PnL.
    void on_market_event(const data::MarketEvent& event);

    // Write all output files.  Creates output_dir if it does not exist.
    void write() const;

    // Exposed for testing.
    double current_equity() const;
    double compute_max_drawdown() const;
    double compute_sharpe() const;

private:
    // ── Position tracking ─────────────────────────────────────────────────

    struct Position {
        double net_qty{0};   // positive = long, negative = short
        double avg_cost{0};  // average entry price per unit
        double realized_pnl{0};
    };

    // Apply a fill to the position, return realized PnL from any closed portion.
    double apply_fill(Position& pos, double fill_qty, double fill_price, matching::OrderSide side);

    double total_realized_pnl() const;
    double total_unrealized_pnl() const;

    // ── Stored records ────────────────────────────────────────────────────

    struct TradeRow {
        uint64_t simulation_ts;
        std::string exchange;
        std::string symbol;
        std::string order_id;
        std::string client_order_id;
        std::string side;
        std::string order_type;
        double qty;
        double price;
        double realized_pnl;  // from this fill only
        double equity;        // total equity after this fill
    };

    struct EquityPoint {
        uint64_t simulation_ts;
        double equity;
    };

    // ── Data ─────────────────────────────────────────────────────────────

    double starting_capital_;
    std::string output_dir_;

    std::unordered_map<std::string, Position> positions_;  // key = "EXCHANGE:SYMBOL"
    std::unordered_map<std::string, double> mid_prices_;   // key = "EXCHANGE:SYMBOL"

    std::vector<TradeRow> trades_;
    std::vector<EquityPoint> equity_curve_;
};

}  // namespace bpt::backtester::results
