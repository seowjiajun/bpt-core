#include "pricer/messaging/publishers/aeron/status_publisher.h"

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <cstddef>

namespace bpt::pricer::messaging::aeron {

StatusPublisher::StatusPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                 const bpt::common::config::StreamConfig& stream) {
    pub_ = bpt::common::aeron::wait_for_publication(aeron, stream.channel, stream.stream_id);
    bpt::common::log::info("[aeron::StatusPublisher] Publication ready on {} stream {}",
                           stream.channel,
                           stream.stream_id);
}

void StatusPublisher::publish_heartbeat(uint64_t timestamp_ns, uint64_t seq_num) {
    if (!pub_)
        return;
    alignas(8) std::byte scratch[SbePricerHeartbeatCodec::kRecommendedScratchSize];
    const auto bytes = hb_codec_.encode(PricerHeartbeatMsg{timestamp_ns, seq_num}, scratch);
    ::aeron::concurrent::AtomicBuffer buffer(reinterpret_cast<uint8_t*>(scratch), bytes.size());
    pub_->offer(buffer, 0, static_cast<int32_t>(bytes.size()));
}

void StatusPublisher::publish_ready(uint64_t timestamp_ns,
                                    uint8_t exchanges_loaded,
                                    uint16_t underlying_count,
                                    uint32_t point_count) {
    if (!pub_)
        return;
    alignas(8) std::byte scratch[SbePricerReadyCodec::kRecommendedScratchSize];
    const PricerReadyMsg msg{timestamp_ns, exchanges_loaded, underlying_count, point_count};
    const auto bytes = ready_codec_.encode(msg, scratch);
    ::aeron::concurrent::AtomicBuffer buffer(reinterpret_cast<uint8_t*>(scratch), bytes.size());
    pub_->offer(buffer, 0, static_cast<int32_t>(bytes.size()));

    bpt::common::log::info("Published PricerReady: exchanges=0x{:02x} underlyings={} points={}",
                           exchanges_loaded,
                           underlying_count,
                           point_count);
}

}  // namespace bpt::pricer::messaging::aeron
