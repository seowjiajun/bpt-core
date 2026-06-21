#include "analytics/messaging/publishers/aeron/toxicity_publisher.h"

#include <cstddef>

namespace bpt::analytics::messaging::aeron {

ToxicityPublisher::ToxicityPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                     const bpt::common::config::StreamConfig& stream)
    : pub_(std::make_unique<bpt::common::aeron::Publisher>(std::move(aeron),
                                                           stream.channel,
                                                           stream.stream_id,
                                                           bpt::common::aeron::Publisher::Policy::kBoundedRetry)) {}

bool ToxicityPublisher::publish(const ToxicityUpdate& update) {
    if (!pub_)
        return false;
    alignas(8) std::byte scratch[PodToxicityCodec::kRecommendedScratchSize];
    const auto bytes = codec_.encode(update, scratch);
    ::aeron::concurrent::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), bytes.size());
    return pub_->offer(ab, 0, static_cast<int32_t>(bytes.size()));
}

}  // namespace bpt::analytics::messaging::aeron
