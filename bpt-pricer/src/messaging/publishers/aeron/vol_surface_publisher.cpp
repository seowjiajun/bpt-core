#include "pricer/messaging/publishers/aeron/vol_surface_publisher.h"

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <cstddef>

namespace bpt::pricer::messaging::aeron {

VolSurfacePublisher::VolSurfacePublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                         const bpt::common::config::StreamConfig& stream) {
    pub_ = bpt::common::aeron::wait_for_publication(aeron, stream.channel, stream.stream_id);
    bpt::common::log::info("[aeron::VolSurfacePublisher] Publication ready on {} stream {}",
                           stream.channel,
                           stream.stream_id);
}

void VolSurfacePublisher::publish(const surface::VolSurfaceGrid& grid, uint64_t timestamp_ns) {
    if (!pub_)
        return;

    // Stack-allocate the scratch buffer per publish — same memory profile
    // as the previous inline-encode shape. The codec owns the encode
    // implementation; this method just composes (codec → Aeron offer).
    alignas(8) std::byte scratch[SbeVolSurfaceCodec::kRecommendedScratchSize];
    const auto bytes = codec_.encode(grid, timestamp_ns, scratch);

    ::aeron::concurrent::AtomicBuffer buffer(reinterpret_cast<uint8_t*>(scratch), bytes.size());
    pub_->offer(buffer, 0, static_cast<int32_t>(bytes.size()));
}

}  // namespace bpt::pricer::messaging::aeron
