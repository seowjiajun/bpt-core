#pragma once

/// @file
/// Aeron+SBE implementation of IVolSurfacePublisher. The Aeron stream
/// is the prod transport for the vol surface — every consumer in the
/// stack (bridge, strategy, radar) subscribes to it. This is what gets
/// constructed by PricerAeronBus::build() at the composition root.
///
/// For the deterministic backtester an InProcessVolSurfacePublisher
/// (separate file, separate bus factory) calls a std::function directly
/// instead of going through SBE encode + Aeron offer.

#include "pricer/messaging/publishers/i_vol_surface_publisher.h"

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bpt::pricer::messaging {

class AeronVolSurfacePublisher : public IVolSurfacePublisher {
public:
    AeronVolSurfacePublisher(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    void publish(const surface::VolSurfaceGrid& grid, uint64_t timestamp_ns) override;

private:
    std::shared_ptr<aeron::Publication> pub_;
};

}  // namespace bpt::pricer::messaging
