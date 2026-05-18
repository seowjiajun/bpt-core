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
#include "md_gateway/messaging/publishers/aeron/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/aeron/instrument_stats_publisher.h"
#include "md_gateway/messaging/publishers/md_publisher.h"
#include "md_gateway/messaging/subscribers/aeron/md_control_subscriber.h"

namespace bpt::md_gateway::messaging {

MdGatewayBus MdGatewayAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    MdGatewayBus bus;
    bus.control_sub = std::make_unique<aeron::MdControlSubscriber>(aeron,
                                                                      settings.aeron.md_control.channel,
                                                                      settings.aeron.md_control.stream_id);
    bus.md_pub =
        std::make_shared<MdPublisher>(aeron, settings.aeron.md_data.channel, settings.aeron.md_data.stream_id);
    bus.ack_pub = std::make_unique<aeron::AckPublisher>(aeron,
                                                         settings.aeron.md_ack_hb.channel,
                                                         settings.aeron.md_ack_hb.stream_id);
    bus.funding_pub = std::make_shared<aeron::FundingRatePublisher>(aeron,
                                                                     settings.aeron.funding_rate.channel,
                                                                     settings.aeron.funding_rate.stream_id);
    bus.stats_pub = std::make_shared<aeron::InstrumentStatsPublisher>(aeron,
                                                                       settings.aeron.instrument_stats.channel,
                                                                       settings.aeron.instrument_stats.stream_id);
    return bus;
}

}  // namespace bpt::md_gateway::messaging
