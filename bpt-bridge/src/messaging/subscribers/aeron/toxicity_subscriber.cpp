#include "bridge/messaging/subscribers/aeron/toxicity_subscriber.h"

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

#include <cstring>

namespace bpt::bridge::messaging::aeron {

ToxicitySubscriber::ToxicitySubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                       const std::string& channel,
                                       int32_t stream_id) {
    sub_ = bpt::common::aeron::wait_for_subscription(std::move(aeron), channel, stream_id);
    bpt::common::log::info("[bridge/Toxicity] subscribed on {} stream {}", channel, stream_id);
}

int ToxicitySubscriber::poll(int fragment_limit) {
    if (!sub_)
        return 0;
    return sub_->poll(
        [this](::aeron::AtomicBuffer& buffer,
               ::aeron::util::index_t offset,
               ::aeron::util::index_t length,
               ::aeron::Header& /*hdr*/) {
            if (static_cast<std::size_t>(length) != sizeof(bpt::analytics::messaging::ToxicityUpdate))
                return;
            bpt::analytics::messaging::ToxicityUpdate u;
            std::memcpy(&u, buffer.buffer() + offset, sizeof(u));
            if (handler_)
                handler_(u);
        },
        fragment_limit);
}

}  // namespace bpt::bridge::messaging::aeron
