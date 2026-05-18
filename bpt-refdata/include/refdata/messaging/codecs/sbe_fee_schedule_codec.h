#pragma once

#include "bpt_common/codec/codec.h"
#include "refdata/model/funding_rate.h"  // FeeScheduleState

#include <cstddef>
#include <span>

namespace bpt::refdata::messaging {

class SbeFeeScheduleCodec {
public:
    std::span<const std::byte> encode(const model::FeeScheduleState&, std::span<std::byte> scratch);
    model::FeeScheduleState   decode(std::span<const std::byte>);

    static constexpr std::size_t kRecommendedScratchSize = 128;
};

static_assert(bpt::common::codec::Codec<SbeFeeScheduleCodec, model::FeeScheduleState>);

}  // namespace bpt::refdata::messaging
