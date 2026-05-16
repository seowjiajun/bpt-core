#pragma once

/// \file
/// \brief MarketColor — bpt-radar's per-(exchange, underlying) market-color snapshot.
///
/// Published on Aeron stream `market_color` (typically 6002). One message per
/// (exchange, underlying) tuple per publish interval. POD struct rather than
/// SBE since this is an internal C++-to-C++ contract today; promote to SBE
/// if a non-C++ consumer (e.g. a Python notebook over Aeron) ever appears.
///
/// Fields are grouped into domain sections (options_, perp_, flow_, regime_,
/// liquidity_). Each section is independently populated by a corresponding
/// analysis module; absent / not-yet-computed fields stay at their NaN/0
/// defaults. Consumers should `isnan()` doubles before rendering.
///
/// Today only the options_* section is wired. Future analysis modules
/// (perp basis, flow imbalance, vol regime, liquidity regime) plug into
/// the corresponding section without breaking the wire layout — new fields
/// append at the end of the section with NaN defaults, old consumers stay
/// happy reading the same prefix.

#include <cstdint>
#include <limits>

namespace bpt::radar::messaging {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

struct __attribute__((packed)) MarketColor {
    /// \name Frame identity.
    /// @{
    uint64_t timestamp_ns;
    uint8_t exchange_id;  ///< bpt::messages::ExchangeId::Value
    char underlying[8];   ///< null-padded ASCII (e.g. "BTC\0\0\0\0\0")
    /// @}

    // ─── Options ────────────────────────────────────────────────────────
    // Populated from bpt-pricer's VolSurface joined with InstrumentStats OI.
    // All vol values are annualised (matching VolSurface convention).
    //
    // Front-month expiry summary (closest non-expired):
    uint32_t options_front_expiry_yyyymmdd{0};
    double options_front_time_to_expiry_y{kNan};
    double options_front_forward_price{kNan};
    double options_front_atm_iv{kNan};      ///< IV at strike closest to forward
    double options_front_rr_25d{kNan};      ///< IV(25Δ call) − IV(25Δ put), vol units
    double options_front_skew_slope{kNan};  ///< dIV / d(log-strike) at ATM

    // Back-month expiry summary (latest-dated on the surface):
    uint32_t options_back_expiry_yyyymmdd{0};
    double options_back_time_to_expiry_y{kNan};
    double options_back_atm_iv{kNan};

    /// Term spread = back ATM IV − front ATM IV. Positive = contango.
    double options_term_spread{kNan};

    /// OI-derived (NaN when no InstrumentStats has joined any strike).
    double options_gex{kNan};               ///< Σ ±gamma·OI; sign by call/put
    double options_max_pain_strike{kNan};   ///< strike minimising in-the-money payout at front expiry
    double options_total_oi{kNan};          ///< Σ OI across strikes contributing to GEX

    // Diagnostics for the options section:
    uint32_t options_strike_count{0};       ///< total IvPoints seen on this surface (call + put)
    uint32_t options_expiry_count{0};       ///< distinct expiries observed
    uint32_t options_strikes_with_oi{0};    ///< strikes that had matched OI from InstrumentStats

    // ─── Perp / futures ─────────────────────────────────────────────────
    // Populated from md-gateway's FundingRate stream joined to this
    // underlying's perp instrument via the refdata snapshot mapping.
    // Funding-related fields are NaN/0 if no perp exists for this
    // underlying or the venue hasn't yet published a funding update.
    double perp_funding_rate_8h{kNan};      ///< 8-hour funding rate, decimal (e.g. 0.0001 = 1 bp)
    uint64_t perp_next_funding_ts_ns{0};    ///< wall-time ns of next funding tick (0 = unknown)
    /// (mark - index) / index × 1e4. Positive = perp trades premium to spot
    /// (contango / longs paying for leverage). Negative = backwardation /
    /// shorts paying. NaN when md-gateway hasn't pushed mark+index for this
    /// underlying's perp, or when the snapshot is stale (>30s).
    double perp_basis_bps{kNan};
    double perp_mark_price{kNan};           ///< mark price from md-gateway InstrumentStats
    double perp_index_price{kNan};          ///< index/spot price from md-gateway InstrumentStats

    // ─── Flow ───────────────────────────────────────────────────────────
    // Per-perp aggressor flow over a fixed rolling window (5 min). Populated
    // by joining MdTrade frames against perp_id_by_key_; NaN/0 when the perp
    // hasn't traded in the window or refdata hasn't joined a perp yet.
    double flow_buy_notional_5m{kNan};   ///< Σ (price × qty) where side = BUY (aggressor hit ask)
    double flow_sell_notional_5m{kNan};  ///< Σ (price × qty) where side = SELL (aggressor hit bid)
    /// (buy − sell) / (buy + sell) notional, range [−1, +1]. Positive = aggressors
    /// lifting offers more than hitting bids. NaN when notional total is 0.
    double flow_imbalance_5m{kNan};
    uint32_t flow_trade_count_5m{0};     ///< # trades inside the window

    // ─── Vol regime ─────────────────────────────────────────────────────
    // Annualised realized vol of the joined perp's mid, computed over a
    // 1h rolling window of ~5s-throttled samples. Frontend joins this with
    // options_front_atm_iv to surface the variance risk premium and a
    // human-friendly regime label, so the classifier stays out of the wire
    // schema and can evolve without forcing consumers to re-deploy.
    double regime_realized_vol_1h{kNan};   ///< annualised, decimal (0.5 = 50%)
    uint32_t regime_sample_count{0};       ///< # mid samples inside the window
};

}  // namespace bpt::radar::messaging
