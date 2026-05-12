#include "strategy/messaging/dashboard_control_subscriber.h"

#include <chrono>
#include <thread>

namespace bpt::strategy::messaging {

DashboardControlSubscriber::DashboardControlSubscriber(std::shared_ptr<aeron::Aeron> aeron,
                                                       const std::string& channel,
                                                       int stream_id) {
    // Same 5-second poll-and-wait as the inline construction the app
    // used previously. The dashboard publisher may not be up yet; if we
    // can't bind, log at the call site (is_ready() = false) and the
    // poll loop becomes a no-op.
    const int64_t reg_id = aeron->addSubscription(channel, stream_id);
    for (int i = 0; i < 500; ++i) {
        sub_ = aeron->findSubscription(reg_id);
        if (sub_)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int DashboardControlSubscriber::poll(int fragment_limit) {
    if (!sub_)
        return 0;
    return sub_->poll(
        [this](aeron::AtomicBuffer& buffer,
               aeron::util::index_t offset,
               aeron::util::index_t length,
               aeron::Header& /*hdr*/) {
            if (length < 1)
                return;
            const uint8_t cmd = *reinterpret_cast<const uint8_t*>(buffer.buffer() + offset);
            if (on_command)
                on_command(cmd);
        },
        fragment_limit);
}

}  // namespace bpt::strategy::messaging
