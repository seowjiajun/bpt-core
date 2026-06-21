#include "bridge/messaging/publishers/aeron/console_control_publisher.h"

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::bridge::messaging::aeron {

ConsoleControlPublisher::ConsoleControlPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                                 const bpt::common::config::StreamConfig& stream) {
    pub_ = bpt::common::aeron::wait_for_publication(std::move(aeron), stream.channel, stream.stream_id);
    bpt::common::log::info("[bridge/Control] publication ready on stream {}", stream.stream_id);
}

void ConsoleControlPublisher::publish_halt() {
    publish_byte(0x00);
}

void ConsoleControlPublisher::publish_resume() {
    publish_byte(0x01);
}

void ConsoleControlPublisher::publish_byte(uint8_t cmd) {
    if (!pub_)
        return;
    ::aeron::AtomicBuffer buf(&cmd, 1);
    const auto result = pub_->offer(buf, 0, 1);
    if (result < 0)
        bpt::common::log::warn("[bridge/Control] offer failed: {}", result);
}

}  // namespace bpt::bridge::messaging::aeron
