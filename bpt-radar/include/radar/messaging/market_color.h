#pragma once

/// \file
/// \brief MarketColor — bpt-radar's per-(exchange, underlying) options-color snapshot.
///
/// Published on Aeron stream `market_color` (typically 6002). One message per
/// (exchange, underlying) tuple per publish interval. POD struct rather than
/// SBE since this is an internal C++-to-C++ contract today; promote to SBE
/// if a non-C++ consumer (e.g. a Python notebook over Aeron) ever appears.
///
/// All vol values are annualised (matching VolSurface convention). NaN
/// sentinels indicate "could not compute" — typically because the surface
/// for that (exchange, underlying) had fewer than two strikes spanning the
/// forward, or fewer than two distinct expiries.

#include <cstdint>
#include <limits>

namespace bpt::radar::messaging {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

struct __attribute__((packed)) MarketColor {
    uint64_t timestamp_ns;
    uint8_t exchange_id;         ///< bpt::messages::ExchangeId::Value
    char underlying[8];          ///< null-padded ASCII (e.g. "BTC\0\0\0\0\0")

    /// \name Front-month expiry summary (closest non-expired).
    /// @{
    uint32_t front_expiry_yyyymmdd{0};
    double front_time_to_expiry_y{kNan};
    double front_forward_price{kNan};
    double front_atm_iv{kNan};      ///< IV at strike closest to forward
    double front_rr_25d{kNan};      ///< IV(25Δ call) − IV(25Δ put), vol units
    double front_skew_slope{kNan};  ///< dIV / d(log-strike) at ATM
    /// @}

    /// \name Back-month expiry summary (latest-dated on the surface).
    /// @{
    uint32_t back_expiry_yyyymmdd{0};
    double back_time_to_expiry_y{kNan};
    double back_atm_iv{kNan};
    /// @}

    /// \brief Term spread = back_atm_iv − front_atm_iv. Positive = contango.
    double term_spread{kNan};

    /// \name OI-derived metrics.
    /// Available when InstrumentStats joined cleanly to surface points.
    /// @{
    double gex{kNan};            ///< Σ gamma·OI across strikes (NaN if no OI data)
    double max_pain_strike{kNan};///< strike minimising in-the-money payout at front expiry
    double total_oi{kNan};       ///< Σ OI across strikes contributing to GEX
    /// @}

    /// \name Diagnostics.
    /// @{
    uint32_t strike_count{0};    ///< total IvPoints seen on this surface (call + put)
    uint32_t expiry_count{0};    ///< distinct expiries observed
    uint32_t strikes_with_oi{0}; ///< strikes that had matched OI from InstrumentStats
    /// @}
};

}  // namespace bpt::radar::messaging
