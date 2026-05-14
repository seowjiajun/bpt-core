#pragma once

#include "bridge/ws/message_encoder.h"

#include <cstdint>

namespace bpt::bridge {

// Running position state derived from a stream of fills.
//
// Tracks net qty / avg entry and the cumulative realized PnL since session
// start. Absolute account equity is sourced from order-gateway AccountSnapshots
// in the dashboard — this class deliberately has no notion of a starting
// capital baseline.
class PositionTracker {
public:
    PositionTracker() = default;

    struct FillResult {
        double realized_pnl;    // realized on this fill (0 unless closing/reducing)
        double cumulative_pnl;  // running total of realized PnL since session start
        double net_qty;
        double avg_entry;
    };

    FillResult apply(encode::Side side, double qty, double price);

    double net_qty() const noexcept { return net_qty_; }
    double avg_entry() const noexcept { return avg_entry_; }
    double cumulative_pnl() const noexcept { return cumulative_pnl_; }

    // Mark-to-market PnL given the current market price.
    double unrealized_pnl(double mark_price) const noexcept {
        if (net_qty_ == 0.0)
            return 0.0;
        return (mark_price - avg_entry_) * net_qty_;
    }

private:
    double cumulative_pnl_{0.0};
    double net_qty_{0.0};
    double avg_entry_{0.0};
};

}  // namespace bpt::bridge
