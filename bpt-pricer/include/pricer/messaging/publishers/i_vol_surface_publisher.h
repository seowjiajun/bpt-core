#pragma once

/// @file
/// IVolSurfacePublisher — port interface for the vol-surface output channel.
///
/// Lets PricerService publish a `surface::VolSurfaceGrid` without knowing
/// the transport. The Aeron+SBE concrete is in
/// `aeron_vol_surface_publisher.h`; an in-process concrete (direct
/// std::function dispatch, no encode) is the obvious sibling for the
/// deterministic backtester. Both are constructed at the composition root
/// (PricerAeronBus / future InProcessBus); the service only sees the port.
///
/// Vtable cost: one virtual dispatch per publish call. Vol surface
/// publishes are off the MD hot path (one per surface rebuild, ~Hz
/// cadence), so the indirection is invisible against SBE encode + Aeron
/// offer. The MdPublisher CRTP precedent in project_mdgateway_hexagonal
/// is the right shape for hot-path publishers; this isn't one of them.

#include "pricer/surface/vol_surface_grid.h"

#include <cstdint>

namespace bpt::pricer::messaging {

class IVolSurfacePublisher {
public:
    virtual ~IVolSurfacePublisher() = default;

    /// Publish a fully-built vol surface grid. `timestamp_ns` is the
    /// wall-clock instant at which the grid is consistent (typically
    /// "now" at publish time; backtest replay may inject simulated
    /// timestamps).
    virtual void publish(const surface::VolSurfaceGrid& grid, uint64_t timestamp_ns) = 0;
};

}  // namespace bpt::pricer::messaging
