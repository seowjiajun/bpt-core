#pragma once

#include "backtester/config/settings.h"
#include "backtester/data/market_event.h"
#include "backtester/matching/open_order.h"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::backtester::results {

// Post-fill markout horizons. Markout = (mid_at_target - fill_price) /
// fill_price * 1e4, sign-flipped on SELL so a positive value always
// means "you got a good fill" (price moved with you, not against).
// 50 ms / 1 s / 5 s / 30 s — the standard maker adverse-selection grid.
inline constexpr std::array<int64_t, 4> kMarkoutHorizonsNs = {
    50'000'000LL,
    1'000'000'000LL,
    5'000'000'000LL,
    30'000'000'000LL,
};
inline constexpr std::array<const char*, 4> kMarkoutHorizonLabels = {
    "50ms",
    "1s",
    "5s",
    "30s",
};

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
    // Identity fields the archive list can use to differentiate runs without
    // opening the detail view. simulation_start/end are ISO strings copied
    // straight from the simulation config. wallclock_start_ns is captured
    // here at construction; wallclock_duration_ms is computed at write() time.
    struct RunMetadata {
        std::string simulation_start;  // ISO 8601, e.g. "2026-04-25T00:00:00Z"
        std::string simulation_end;
        std::vector<std::string> instruments;  // e.g. ["HYPERLIQUID:APE"]
        // Run identity. All optional — when empty the run still records,
        // it just can't be diff'd against a peer or replayed exactly.
        std::string strategy_name;  // e.g. "AvellanedaStoikov"
        std::string params_hash;    // sha256 of strategy config (8 chars typical)
        std::string git_sha;        // `git rev-parse HEAD` (7 chars typical)
        // Path to the strategy params file used for this run. When set,
        // ResultsCollector::write() copies it into the run dir as
        // `params.toml` so the dashboard can read the actual param values
        // for sweep visualisation. Empty = no copy.
        std::string params_file;
    };

    // Compose a deterministic run_id from the metadata + window. Used for
    // the on-disk output directory and as the primary key in archive
    // tooling. Falls back to "{start}_{end}" if the identity fields are
    // empty so older runs and ad-hoc invocations still produce a path.
    static std::string compose_run_id(const RunMetadata& m, const std::string& start_tag, const std::string& end_tag);

    ResultsCollector(double starting_capital,
                     std::string output_dir,
                     RunMetadata metadata = {},
                     std::unordered_map<std::string, config::ResultsConfig::FeeRates> fees_by_venue = {});

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
        // Holding-period accounting: ts of the fill that opened the
        // current non-flat run. Reset to 0 each time net_qty returns
        // to flat. We only track closed round-trips — an unflat
        // position at backtest end is excluded from stats.
        uint64_t open_ts_ns{0};
    };

    struct RoundTrip {
        uint64_t open_ts_ns;
        uint64_t close_ts_ns;
        // Realised PnL accumulated across all fills in this round-trip.
        double realized_pnl;
    };

    // Apply a fill to the position, return realized PnL from any closed portion.
    double apply_fill(Position& pos, double fill_qty, double fill_price, matching::OrderSide side);

    double total_realized_pnl() const;
    double total_unrealized_pnl() const;

    // ── Output writers — called by write() in sequence ───────────────────
    // Split out of the original 240-line write() so each output file owns
    // a self-contained, ~30–60 line function. Failures in copy_params_file
    // are swallowed (warn-only) because the strategy params file is a
    // diagnostic aid, not a correctness input; the rest throw if the
    // target file can't be opened.
    void copy_params_file() const;
    void write_trades_csv() const;
    void write_pnl_curve_csv() const;
    void write_summary_json() const;

    // ── Stored records ────────────────────────────────────────────────────

    struct TradeRow {
        uint64_t simulation_ts;
        std::string exchange;
        std::string symbol;
        std::string order_id;
        std::string client_order_id;
        std::string side;
        std::string order_type;
        std::string liquidity;  // "MAKER" or "TAKER" — drives fee_paid below
        double qty;
        double price;
        double realized_pnl;  // from this fill only (gross, fee not deducted here)
        double fee_paid;      // notional × fee_bps / 1e4 — already deducted from `equity`
        double equity;        // total equity after this fill (NET of fees)
        // Markouts in basis points, sign-corrected so positive = good fill.
        // Filled async as later MD events cross each horizon's target_ts.
        // Slot remains kUnresolved if data ran out before the horizon hit.
        std::array<double, 4> markouts_bps{kUnresolved, kUnresolved, kUnresolved, kUnresolved};
    };

    // Sentinel for an unresolved markout. Chosen so any real bps value
    // (rare to exceed ±10000 in practice) is unambiguous.
    static constexpr double kUnresolved = -1e18;

    struct EquityPoint {
        uint64_t simulation_ts;
        double equity;
    };

    // ── Data ─────────────────────────────────────────────────────────────

    double starting_capital_;
    std::string output_dir_;
    RunMetadata metadata_;
    uint64_t wallclock_start_ns_{0};

    // Per-venue maker/taker fee table — looked up per fill via
    // FillReport.exchange + FillReport.liquidity_role. Empty table means
    // "no fees charged" (legitimate for fees-disabled tests). Missing
    // venue at fill time logs a warning once per (venue, role) pair and
    // charges 0 — a misconfig should be loud, not silently miscompute.
    std::unordered_map<std::string, config::ResultsConfig::FeeRates> fees_by_venue_;
    double total_fees_paid_{0.0};
    // Cache of (exchange, role) pairs we've already warned about so
    // logs aren't spammed once per fill.
    mutable std::unordered_map<std::string, bool> missing_fee_warned_;

    std::unordered_map<std::string, Position> positions_;  // key = "EXCHANGE:SYMBOL"
    std::unordered_map<std::string, double> mid_prices_;   // key = "EXCHANGE:SYMBOL"

    std::vector<TradeRow> trades_;
    std::vector<EquityPoint> equity_curve_;
    std::vector<RoundTrip> round_trips_;
    // Realised PnL accumulator that resets at each round-trip close so
    // we can attribute the closed round's PnL.
    std::unordered_map<std::string, double> open_realized_;

    // Pending markout resolutions — populated on each fill, drained on
    // each subsequent market event whose ts crosses the target. Linear
    // scan is fine: the queue stays small (≤ 4 × in-flight unresolved
    // fills) and per-event resolution is amortised cheap.
    struct PendingMarkout {
        std::size_t trade_idx;
        std::size_t horizon_idx;
        uint64_t target_ts_ns;
    };
    std::deque<PendingMarkout> pending_markouts_;
};

}  // namespace bpt::backtester::results
