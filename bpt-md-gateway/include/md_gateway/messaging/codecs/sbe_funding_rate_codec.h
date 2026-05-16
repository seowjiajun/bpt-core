#pragma once

#include "bpt_common/codec/codec.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"  // FundingRateUpdate

#include <cstddef>
#include <span>

namespace bpt::md_gateway::messaging {

class SbeFundingRateCodec {
public:
    std::span<const std::byte> encode(const FundingRateUpdate&, std::span<std::byte> scratch);
    FundingRateUpdate           decode(std::span<const std::byte>);

    static constexpr std::size_t kRecommendedScratchSize = 128;
};

static_assert(bpt::common::codec::Codec<SbeFundingRateCodec, FundingRateUpdate>);

}  // namespace bpt::md_gateway::messaging
