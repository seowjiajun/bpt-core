#include "bridge/messaging/subscribers/portfolio_snapshot_subscriber.h"

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::bridge::messaging {

PortfolioSnapshotSubscriber::PortfolioSnapshotSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                                         const std::string& channel,
                                                         int32_t stream_id) {
    sub_ = bpt::common::aeron::wait_for_subscription(std::move(aeron), channel, stream_id);
    bpt::common::log::info("[bridge/Portfolio] subscribed on {} stream {}", channel, stream_id);

    // FragmentAssembler reassembles multi-fragment Aeron messages before
    // invoking our user-facing handler with the complete JSON payload.
    assembler_ = std::make_unique<::aeron::FragmentAssembler>(
        [this](::aeron::AtomicBuffer& buffer,
               ::aeron::util::index_t offset,
               ::aeron::util::index_t length,
               ::aeron::Header& /*hdr*/) {
            if (!handler_)
                return;
            std::string_view json(reinterpret_cast<const char*>(buffer.buffer() + offset),
                                  static_cast<std::size_t>(length));
            handler_(json);
        });
}

int PortfolioSnapshotSubscriber::poll(int fragment_limit) {
    if (!sub_ || !assembler_)
        return 0;
    return sub_->poll(assembler_->handler(), fragment_limit);
}

}  // namespace bpt::bridge::messaging
