#pragma once

/// \file
/// \brief Plain-data view of a single IV-surface strike, decoupled from SBE.
///
/// Pulled out of the SBE iterator at the bus boundary so analysis modules can
/// be unit-tested with hand-built vectors. Mirrors the field set of
/// VolSurface's `points` group, plus the joined open-interest from
/// InstrumentStats. open_interest is NaN when no matching stats update has
/// been seen yet for that instrument.

#include <cstdint>
#include <limits>

namespace bpt::radar::analysis {

struct SurfacePoint {
    uint64_t instrument_id{0};
    uint32_t expiry_yyyymmdd{0};
    double strike_price{0.0};
    int option_side{0};  ///< 0 = call, 1 = put (matches OptionSide enum order)
    double implied_vol{0.0};
    double forward_price{0.0};
    double time_to_expiry_y{0.0};
    double delta{0.0};
    double gamma{0.0};
    double open_interest{std::numeric_limits<double>::quiet_NaN()};
};

constexpr int kSideCall = 0;
constexpr int kSidePut = 1;

}  // namespace bpt::radar::analysis
