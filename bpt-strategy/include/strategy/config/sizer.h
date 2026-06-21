#pragma once

#include "strategy/venue/min_order_value.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace bpt::strategy::config {

// Shared equity-fraction sizing logic used by AS and FVMM.
struct Sizer {
    double order_qty{0.0};
    double order_qty_fraction{0.0};
    double order_qty_min{0.0};
    double max_inventory{0.0};
    double max_inventory_fraction{0.0};

    [[nodiscard]] double effective_qty(double last_mid, double lot_size,
                                       int64_t equity_e8, const std::string& exchange) const {
        double qty = order_qty;
        if (order_qty_fraction > 0.0 && equity_e8 > 0 && last_mid > 0.0) {
            const double equity_usd = static_cast<double>(equity_e8) / 1e8;
            const double floor_qty = (order_qty_min > 0.0) ? order_qty_min : lot_size;
            qty = std::max(floor_qty, order_qty_fraction * equity_usd / last_mid);
        }
        return bpt::strategy::venue::bump_qty_for_min_notional(
            qty, last_mid, lot_size, bpt::strategy::venue::min_notional_usd(exchange));
    }

    [[nodiscard]] double effective_max_inventory(double last_mid, int64_t equity_e8) const noexcept {
        if (max_inventory_fraction > 0.0 && equity_e8 > 0 && last_mid > 0.0)
            return max_inventory_fraction * static_cast<double>(equity_e8) / 1e8 / last_mid;
        return max_inventory;
    }
};

}  // namespace bpt::strategy::config
