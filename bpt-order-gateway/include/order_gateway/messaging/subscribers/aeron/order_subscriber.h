#pragma once

#include "order_gateway/messaging/codecs/sbe_order_command_codec.h"
#include "order_gateway/messaging/subscribers/api/order_subscriber.h"

#include <Aeron.h>

#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/aeron/subscriber.h>
#include <memory>
#include <string>

namespace bpt::order_gateway::messaging::aeron {

/// \brief Aeron-backed concrete for api::OrderSubscriber.
///
/// Subscribes to the order-control stream and dispatches each decoded
/// SBE fragment to the matching `on_*` callback. Single-threaded
/// contract — poll() drives from the main poll loop.
class OrderSubscriber final : public api::OrderSubscriber {
public:
    OrderSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    /// \copydoc api::OrderSubscriber::poll
    int poll(int fragment_limit = 10) override;

private:
    void handle_fragment(::aeron::AtomicBuffer& buf,
                         ::aeron::util::index_t offset,
                         ::aeron::util::index_t length,
                         ::aeron::Header& hdr);

    SbeOrderCommandCodec codec_;
    std::unique_ptr<bpt::common::aeron::Subscriber> subscription_;
};

}  // namespace bpt::order_gateway::messaging::aeron
