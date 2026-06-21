#pragma once

#include "bpt_common/codec/codec.h"
#include "order_gateway/messaging/publishers/api/exec_report_publisher.h"

#include <cstddef>
#include <span>

namespace bpt::order_gateway::messaging {

class SbeExecutionReportCodec {
public:
    std::span<const std::byte> encode(const api::ExecReport&, std::span<std::byte> scratch);
    api::ExecReport decode(std::span<const std::byte>);

    static constexpr std::size_t kRecommendedScratchSize = 256;
};

static_assert(bpt::common::codec::Codec<SbeExecutionReportCodec, api::ExecReport>);

}  // namespace bpt::order_gateway::messaging
