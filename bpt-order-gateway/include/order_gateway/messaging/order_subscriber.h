#pragma once

#include "order_gateway/messaging/i_order_control_source.h"

#include <Aeron.h>

#include <memory>
#include <string>
#include <bpt_common/aeron/subscriber.h>

namespace bpt::order_gateway::messaging {

/// \brief Aeron-backed concrete for IOrderControlSource.
///
/// Subscribes to the order-control stream and dispatches each decoded
/// SBE fragment to the matching `on_*` callback. Single-threaded
/// contract — poll() drives from the main poll loop.
class OrderSubscriber final : public IOrderControlSource {
public:
    OrderSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                    const std::string& channel,
                    int stream_id);

    /// \copydoc IOrderControlSource::poll
    int poll(int fragment_limit = 10) override;

private:
    void handle_fragment(aeron::AtomicBuffer& buf,
                         aeron::util::index_t offset,
                         aeron::util::index_t length,
                         aeron::Header& hdr);

    std::unique_ptr<bpt::common::aeron::Subscriber> subscription_;
};

}  // namespace bpt::order_gateway::messaging
