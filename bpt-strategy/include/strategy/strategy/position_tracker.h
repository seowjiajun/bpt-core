#pragma once

#include <messages/ExchangeId.h>
#include <messages/OrderSide.h>

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace bpt::strategy::strategy {

// Tracks net position per (instrument_id, exchange) pair.
// Updated from execution report fills.  Single-threaded — only call from the poll thread.
//
// Quantities are stored in 1e8 fixed-point (same scale as NewOrder.quantity
// and ExecutionReport.filledQty — see order_manager.cpp which encodes qty as
// round(qty_natural * 1e8)).
// Prices are stored as double converted from 1e8 fixed-point.
class PositionTracker {
public:
    struct Position {
        int64_t net_qty{0};        // positive = long, negative = short (1e8 fixed-point)
        double avg_price{0.0};     // average entry price in normal units (e.g. 83000.5)
        double realized_pnl{0.0};  // cumulative realised PnL in quote currency
    };

    // Process a fill.  Only call for FILLED or PARTIAL exec reports.
    // filled_qty : 1e8 fixed-point (rpt.filledQty())
    // fill_price : 1e8 fixed-point (rpt.price())
    void on_fill(uint64_t instrument_id,
                 bpt::messages::ExchangeId::Value exchange_id,
                 bpt::messages::OrderSide::Value side,
                 uint64_t filled_qty,
                 int64_t fill_price);

    // Overwrite a position's net_qty and avg_price from an authoritative
    // exchange source (AccountSnapshot row). Leaves realized_pnl alone
    // — that's a session-running PnL counter accrued from observed
    // fills; the exchange's mark-to-market unrealizedPnl is not an
    // equivalent quantity and overwriting rpnl with it would conflate
    // realized and unrealized.
    //
    // Use cases:
    //   - Startup: strategy restarts with a pre-existing exchange
    //     position that on_fill never saw.
    //   - Runtime resync: reconciler sees divergence > threshold mid-
    //     session — missed exec report, WS reconnect gap, external
    //     position change (liquidation, manual flatten). Seed to the
    //     exchange view rather than ignoring; the exchange is always
    //     authoritative.
    //
    // net_qty_e8 : 1e8 fixed-point, signed (positive = long)
    // avg_price  : natural units. Pass 0.0 if unknown — subsequent
    //              fills will blend starting from 0-avg, producing
    //              incorrect realized_pnl on any close of the seeded
    //              size but avoiding a fabricated avg.
    void seed(uint64_t instrument_id,
              bpt::messages::ExchangeId::Value exchange_id,
              int64_t net_qty_e8,
              double avg_price);

    [[nodiscard]] std::optional<Position> get(uint64_t instrument_id,
                                              bpt::messages::ExchangeId::Value exchange_id) const;

    // Returns net_qty in 1e8 fixed-point (0 if no entry).
    [[nodiscard]] int64_t net_qty(uint64_t instrument_id, bpt::messages::ExchangeId::Value exchange_id) const;

    void clear(uint64_t instrument_id, bpt::messages::ExchangeId::Value exchange_id);
    void clear_all();

private:
    static uint64_t key(uint64_t instrument_id, bpt::messages::ExchangeId::Value exchange_id) noexcept {
        return (instrument_id << 8) | static_cast<uint64_t>(exchange_id);
    }

    std::unordered_map<uint64_t, Position> positions_;
};

}  // namespace bpt::strategy::strategy
