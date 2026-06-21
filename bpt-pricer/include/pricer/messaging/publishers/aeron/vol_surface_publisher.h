#pragma once

/// @file
/// Aeron+SBE implementation of api::VolSurfacePublisher. The Aeron stream
/// is the prod transport for the vol surface — every consumer in the
/// stack (bridge, strategy, radar) subscribes to it. This is what gets
/// constructed by PricerAeronBus::build() at the composition root.
///
/// For the deterministic backtester sim::VolSurfacePublisher (separate
/// file, separate bus factory) calls a std::function directly instead of
/// going through SBE encode + Aeron offer.

#include "pricer/messaging/codecs/sbe_vol_surface_codec.h"
#include "pricer/messaging/publishers/api/vol_surface_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::pricer::messaging::aeron {

class VolSurfacePublisher : public api::VolSurfacePublisher {
public:
    VolSurfacePublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    void publish(const surface::VolSurfaceGrid& grid, uint64_t timestamp_ns) override;

private:
    std::shared_ptr<::aeron::Publication> pub_;
    SbeVolSurfaceCodec codec_;  ///< composed; transport-agnostic encode lives here
};

}  // namespace bpt::pricer::messaging::aeron
