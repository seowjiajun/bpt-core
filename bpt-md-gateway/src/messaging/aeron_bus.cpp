/// \file
/// \brief MdGatewayAeronBus::build — instantiate the prod messaging-port concrete classes.
///
/// One place that maps `settings.aeron.*` channel+stream pairs onto the
/// concrete Aeron-backed classes. Kept tiny on purpose: the app's wiring
/// is a single call (`MdGatewayAeronBus::build`) which the rest of the binary
/// never has to look at again.

#include "md_gateway/messaging/aeron_bus.h"

#include "md_gateway/config/settings.h"
#include "md_gateway/messaging/publishers/aeron/ack_publisher.h"
#include "md_gateway/messaging/subscribers/aeron/md_control_subscriber.h"

namespace bpt::md_gateway::messaging {

MdGatewayBus MdGatewayAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    MdGatewayBus bus;
    bus.control_sub = std::make_unique<aeron::MdControlSubscriber>(aeron, settings.aeron.md_control);
    bus.ack_pub = std::make_unique<aeron::AckPublisher>(aeron, settings.aeron.md_ack_hb);
    return bus;
}

}  // namespace bpt::md_gateway::messaging
