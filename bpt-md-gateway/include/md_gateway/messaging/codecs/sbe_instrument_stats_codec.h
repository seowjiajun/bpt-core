#pragma once

#include "bpt_common/codec/codec.h"
#include "md_gateway/messaging/publishers/api/instrument_stats_publisher.h"  // InstrumentStatsUpdate

#include <cstddef>
#include <span>

namespace bpt::md_gateway::messaging {

class SbeInstrumentStatsCodec {
public:
    std::span<const std::byte> encode(const InstrumentStatsUpdate&, std::span<std::byte> scratch);
    InstrumentStatsUpdate       decode(std::span<const std::byte>);

    static constexpr std::size_t kRecommendedScratchSize = 256;
};

static_assert(bpt::common::codec::Codec<SbeInstrumentStatsCodec, InstrumentStatsUpdate>);

}  // namespace bpt::md_gateway::messaging
