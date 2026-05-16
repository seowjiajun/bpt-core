#pragma once

#include "bpt_common/codec/codec.h"
#include "order_gateway/messaging/publishers/api/account_snapshot_publisher.h"  // adapter::AccountSnapshotData

#include <cstddef>
#include <span>

namespace bpt::order_gateway::messaging {

class SbeAccountSnapshotCodec {
public:
    std::span<const std::byte> encode(const adapter::AccountSnapshotData&, std::span<std::byte> scratch);
    adapter::AccountSnapshotData decode(std::span<const std::byte>);

    // Worst-case: 500 positions × 56B + 32 ccyBalances × 24B + header.
    static constexpr std::size_t kRecommendedScratchSize = 64 * 1024;
};

static_assert(bpt::common::codec::Codec<SbeAccountSnapshotCodec, adapter::AccountSnapshotData>);

}  // namespace bpt::order_gateway::messaging
