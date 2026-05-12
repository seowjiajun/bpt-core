#pragma once

// Per-instrument realized-P&L tracker used by the order-gateway risk
// module. Consumes fill events from the OrderProcessor's exec-event
// drain and maintains:
//   - Session-cumulative realized P&L (in quote currency, assumed USD
//     for cross-venue aggregation purposes).
//   - Daily realized P&L, resetting automatically at UTC midnight on
//     the next fill after the rollover.
//
// Used by the daily-loss kill switch: when daily P&L drops below
// -max_daily_loss_usd, the caller flips RiskChecker.set_trading_enabled
// to false. The latch is not cleared automatically on the next day —
// operators must restart the service or explicitly call a clear hook
// (deliberate: after a breach we want a human to look before resuming).
//
// Price/qty inputs are scaled int64 (1e8 scale), matching ExecutionReport
// and PositionTracker conventions in bpt-strategy. Exposed P&L is
// unscaled double USD.
//
// Thread model: single-writer from OrderProcessor's main thread (same
// thread that drains exec events). No internal synchronisation.

#include <messages/ExchangeId.h>
#include <messages/OrderSide.h>

#include <cstdint>
#include <unordered_map>

namespace bpt::order_gateway::risk {

class PnlTracker {
public:
    PnlTracker() = default;

    // Record a fill. Computes realized P&L contribution using a
    // weighted-average-cost model:
    //   BUY when short  → realize (entry - price) × closed_qty
    //   SELL when long  → realize (price - entry) × closed_qty
    //   Same-side fills just update the position's avg_entry.
    // Updates daily + session totals.
    void on_fill(bpt::messages::ExchangeId::Value exchange,
                 uint64_t instrument_id,
                 bpt::messages::OrderSide::Value side,
                 int64_t price_e8,
                 uint64_t filled_qty_e8,
                 uint64_t fill_ts_ns);

    // Realized P&L for the current UTC day, in USD. Triggers a
    // rollover check first so a stale "yesterday's loss" doesn't keep
    // a halted trading session disabled after midnight without a human
    // looking. (The halt itself isn't cleared here — see the note at
    // the top of this file.)
    [[nodiscard]] double daily_realized_pnl_usd(uint64_t now_ns);

    // Session-cumulative since process start. For reconciliation
    // against exchange-reported AccountSnapshot.
    [[nodiscard]] double session_realized_pnl_usd() const noexcept { return session_pnl_usd_; }

    // Signed net position for a specific (exchange, instrument), in
    // base units × 1e8. Positive = long, negative = short, 0 = flat or
    // never traded. Used by the max_position_usd pretrade gate — the
    // caller multiplies by the order's price to get projected USD
    // exposure after the new order would fill.
    [[nodiscard]] int64_t net_qty_e8(bpt::messages::ExchangeId::Value exchange, uint64_t instrument_id) const noexcept;

private:
    static constexpr double kScale = 1e8;

    struct Position {
        // Stored as signed scaled units (int64) to match PositionTracker
        // semantics in bpt-strategy; positive = long, negative = short.
        int64_t net_qty_e8{0};
        double avg_price_usd{0.0};  // VWAP of currently-open side
    };

    static uint64_t utc_day_of(uint64_t ns);

    void roll_day_if_needed(uint64_t now_ns);

    // Keyed by (exchange_id << 56) | instrument_id — fits 2^56 instruments.
    std::unordered_map<uint64_t, Position> positions_;

    uint64_t current_utc_day_{0};
    double daily_pnl_usd_{0.0};
    double session_pnl_usd_{0.0};
};

}  // namespace bpt::order_gateway::risk
