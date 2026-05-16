#include "analytics/messaging/publishers/aeron_toxicity_publisher.h"

#include <cstddef>

namespace bpt::analytics::messaging {

AeronToxicityPublisher::AeronToxicityPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                               const std::string& channel,
                                               int stream_id)
    : pub_(std::make_unique<bpt::common::aeron::Publisher>(std::move(aeron),
                                                           channel,
                                                           stream_id,
                                                           bpt::common::aeron::Publisher::Policy::kBoundedRetry)) {}

bool AeronToxicityPublisher::publish(const ToxicityUpdate& update) {
    if (!pub_)
        return false;
    alignas(8) std::byte scratch[PodToxicityCodec::kRecommendedScratchSize];
    const auto bytes = codec_.encode(update, scratch);
    aeron::concurrent::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), bytes.size());
    return pub_->offer(ab, 0, static_cast<int32_t>(bytes.size()));
}

}  // namespace bpt::analytics::messaging
