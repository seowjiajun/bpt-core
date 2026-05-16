#include "strategy/messaging/subscribers/toxicity_subscriber.h"

#include <bpt_common/aeron/aeron_utils.h>

namespace bpt::strategy::messaging {

ToxicitySubscriber::ToxicitySubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id)
    : sub_(bpt::common::aeron::wait_for_subscription(std::move(aeron), channel, stream_id)) {}

int ToxicitySubscriber::poll(int fragment_limit) {
    if (!sub_)
        return 0;
    return sub_->poll(
        [this](const aeron::concurrent::AtomicBuffer& buffer,
               aeron::util::index_t offset,
               aeron::util::index_t length,
               const aeron::Header&) {
            if (static_cast<std::size_t>(length) != sizeof(bpt::analytics::messaging::ToxicityUpdate))
                return;
            bpt::analytics::messaging::ToxicityUpdate update;
            std::memcpy(&update, buffer.buffer() + offset, sizeof(update));
            if (on_update)
                on_update(update);
        },
        fragment_limit);
}

}  // namespace bpt::strategy::messaging
