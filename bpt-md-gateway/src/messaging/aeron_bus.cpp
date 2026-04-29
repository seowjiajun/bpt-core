/// \file
/// \brief AeronBus::build — instantiate the prod messaging-port concrete classes.
///
/// One place that maps `settings.aeron.*` channel+stream pairs onto the
/// concrete Aeron-backed classes. Kept tiny on purpose: the app's wiring
/// is a single call (`AeronBus::build`) which the rest of the binary
/// never has to look at again.

#include "md_gateway/messaging/aeron_bus.h"

#include "md_gateway/config/settings.h"
#include "md_gateway/messaging/ack_publisher.h"
#include "md_gateway/messaging/funding_rate_publisher.h"
#include "md_gateway/messaging/md_control_subscriber.h"
#include "md_gateway/messaging/md_publisher.h"

namespace bpt::md_gateway::messaging {

AeronBus AeronBus::build(std::shared_ptr<aeron::Aeron> aeron,
                         const config::Settings& settings) {
    AeronBus bus;
    bus.control_source = std::make_unique<MdControlSubscriber>(
        aeron, settings.aeron.control.channel, settings.aeron.control.stream_id);
    bus.md_sink = std::make_shared<MdPublisher>(
        aeron, settings.aeron.data.channel, settings.aeron.data.stream_id);
    bus.ack_sink = std::make_unique<AckPublisher>(
        aeron, settings.aeron.ack_hb.channel, settings.aeron.ack_hb.stream_id);
    bus.funding_sink = std::make_shared<FundingRatePublisher>(
        aeron, settings.aeron.funding_rate.channel, settings.aeron.funding_rate.stream_id);
    return bus;
}

}  // namespace bpt::md_gateway::messaging
