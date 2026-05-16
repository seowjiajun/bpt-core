#include "radar/messaging/publishers/market_color_publisher.h"

#include <Aeron.h>

namespace bpt::radar::messaging {

using Policy = bpt::common::aeron::Publisher::Policy;

MarketColorPublisher::MarketColorPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                           const std::string& channel,
                                           int stream_id)
    // kBoundedRetry mirrors ToxicityPublisher's policy — periodic snapshot
    // stream, the next interval produces a fresh frame so we don't spin
    // forever on a slow consumer.
    : pub_(std::make_unique<bpt::common::aeron::Publisher>(std::move(aeron),
                                                           channel,
                                                           stream_id,
                                                           Policy::kBoundedRetry)) {}

bool MarketColorPublisher::publish(const MarketColor& color) {
    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(const_cast<MarketColor*>(&color)), sizeof(MarketColor));
    return pub_->offer(ab, 0, static_cast<aeron::util::index_t>(sizeof(MarketColor)));
}

}  // namespace bpt::radar::messaging
