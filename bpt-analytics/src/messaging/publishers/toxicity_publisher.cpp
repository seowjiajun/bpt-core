#include "analytics/messaging/publishers/toxicity_publisher.h"

namespace bpt::analytics::messaging {

ToxicityPublisher::ToxicityPublisher(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id)
    : pub_(std::make_unique<bpt::common::aeron::Publisher>(std::move(aeron),
                                                           channel,
                                                           stream_id,
                                                           bpt::common::aeron::Publisher::Policy::kBoundedRetry)) {}

bool ToxicityPublisher::publish(const ToxicityUpdate& update) {
    if (!pub_)
        return false;
    aeron::concurrent::AtomicBuffer ab(reinterpret_cast<uint8_t*>(const_cast<ToxicityUpdate*>(&update)),
                                       sizeof(update));
    return pub_->offer(ab, 0, static_cast<int32_t>(sizeof(update)));
}

}  // namespace bpt::analytics::messaging
