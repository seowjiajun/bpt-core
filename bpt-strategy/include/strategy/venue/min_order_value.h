#pragma once

/// \file
/// \brief Minimum order-value rules per venue, applied at order construction.
///
/// Some venues enforce a minimum *notional* per order (`price × qty ≥ floor`)
/// that's independent of tick size and lot size. The bpt-refdata Instrument
/// today carries tick_size and lot_size; the min-notional floor is **not** on
/// the wire because most venues don't expose it programmatically:
///
///   HL:       venue-wide $10 floor (documented in HL API, not in /info)
///   Binance:  per-symbol MIN_NOTIONAL filter on /api/v3/exchangeInfo
///   OKX:      per-instrument minSz (base-qty) on /api/v5/public/instruments
///   Deribit:  per-instrument min_trade_amount
///
/// Until a future refactor publishes this on RefDataSnapshot/Delta (would
/// require an SBE schema bump + per-adapter wiring), strategies call into
/// this header to gate qty on the per-venue floor at order-construction time.
/// One number, one venue — keeping it here as a constant rather than a wire
/// field avoids premature abstraction.

#include <algorithm>
#include <cmath>
#include <string>

namespace bpt::strategy::venue {

/// \brief Per-venue minimum order *notional* (in quote currency, ~USD).
///
/// Returns 0 when the venue doesn't impose a venue-wide floor (Binance/OKX
/// rules are per-symbol and belong in refdata when they land). 0 also means
/// "no adjustment needed" in the helpers below.
inline double min_notional_usd(const std::string& venue) {
    if (venue == "HYPERLIQUID") {
        // HL universal min: orders < $10 reject with
        // "Order must have minimum value of $10. asset=N".
        // Update if HL publishes a different floor.
        return 10.0;
    }
    return 0.0;
}

/// \brief Bump `qty` up to satisfy the venue's min-notional, rounded up to
/// the next `lot_size` increment.
///
/// Returns the larger of `qty` and the computed min-qty. When the venue has
/// no floor (0), returns `qty` unchanged. Bails gracefully when price or
/// lot_size are non-positive (would otherwise divide by zero).
///
/// Adds a 5% headroom over the bare floor: HL rejected orders that
/// landed at exactly $10.01 during validation (the venue likely re-checks
/// against an internal price snapshot that lags the wire price by a tick
/// or two — at $10.01, a single 0.1% mark drift drops us under). 5%
/// covers that window comfortably without inflating the order much
/// beyond the operator's intent.
inline double bump_qty_for_min_notional(double qty,
                                        double price,
                                        double lot_size,
                                        double min_notional_usd) {
    if (min_notional_usd <= 0.0 || price <= 0.0 || lot_size <= 0.0)
        return qty;
    constexpr double kHeadroom = 1.05;
    const double min_qty = (min_notional_usd * kHeadroom) / price;
    const double min_qty_rounded = std::ceil(min_qty / lot_size) * lot_size;
    return std::max(qty, min_qty_rounded);
}

}  // namespace bpt::strategy::venue
