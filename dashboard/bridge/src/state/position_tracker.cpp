#include "bridge/state/position_tracker.h"

#include <cmath>

namespace bridge {

PositionTracker::FillResult PositionTracker::apply(encode::Side side, double qty, double price) {
    const double signed_qty = (side == encode::Side::Buy ? qty : -qty);
    const double prev_net = net_qty_;
    const double next_net = prev_net + signed_qty;

    double realized = 0.0;

    if (prev_net == 0.0) {
        // Opening a fresh position
        avg_entry_ = price;
    } else if ((prev_net > 0 && next_net < 0) || (prev_net < 0 && next_net > 0)) {
        // Flipping sides — realise PnL on the closing portion, then reset entry
        realized = (price - avg_entry_) * prev_net;
        avg_entry_ = price;
    } else if (std::abs(next_net) > std::abs(prev_net)) {
        // Adding to the existing position — weighted average
        avg_entry_ = (avg_entry_ * std::abs(prev_net) + price * qty) / std::abs(next_net);
    } else {
        // Reducing an existing position — realise PnL on the closed portion
        const double closed = std::abs(prev_net) - std::abs(next_net);
        realized = (price - avg_entry_) * (prev_net > 0 ? closed : -closed);
    }

    if (next_net == 0.0) {
        avg_entry_ = 0.0;
    }

    net_qty_ = next_net;
    cumulative_pnl_ += realized;

    return {realized, cumulative_pnl_, net_qty_, avg_entry_};
}

}  // namespace bridge
