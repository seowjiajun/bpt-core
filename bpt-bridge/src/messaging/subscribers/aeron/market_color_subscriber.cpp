#include "bridge/messaging/subscribers/aeron/market_color_subscriber.h"

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

#include <cstring>

namespace bpt::bridge::messaging::aeron {

MarketColorSubscriber::MarketColorSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                             const std::string& channel,
                                             int32_t stream_id) {
    sub_ = bpt::common::aeron::wait_for_subscription(std::move(aeron), channel, stream_id);
    bpt::common::log::info("[bridge/MarketColor] subscribed on {} stream {}", channel, stream_id);
}

int MarketColorSubscriber::poll(int fragment_limit) {
    if (!sub_)
        return 0;
    return sub_->poll(
        [this](::aeron::AtomicBuffer& buffer,
               ::aeron::util::index_t offset,
               ::aeron::util::index_t length,
               ::aeron::Header& /*hdr*/) {
            if (static_cast<std::size_t>(length) != sizeof(bpt::radar::messaging::MarketColor))
                return;
            bpt::radar::messaging::MarketColor mc;
            std::memcpy(&mc, buffer.buffer() + offset, sizeof(mc));
            if (handler_)
                handler_(mc);
        },
        fragment_limit);
}

}  // namespace bpt::bridge::messaging::aeron
