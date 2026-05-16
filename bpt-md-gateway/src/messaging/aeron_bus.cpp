/// \file
/// \brief AeronBus::build — instantiate the prod messaging-port concrete classes.
///
/// One place that maps `settings.aeron.*` channel+stream pairs onto the
/// concrete Aeron-backed classes. Kept tiny on purpose: the app's wiring
/// is a single call (`AeronBus::build`) which the rest of the binary
/// never has to look at again.

#include "md_gateway/messaging/aeron_bus.h"

#include "md_gateway/config/settings.h"
#include "md_gateway/messaging/publishers/ack_publisher.h"
#include "md_gateway/messaging/publishers/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/instrument_stats_publisher.h"
#include "md_gateway/messaging/publishers/md_publisher.h"
#include "md_gateway/messaging/subscribers/md_control_subscriber.h"

namespace bpt::md_gateway::messaging {

AeronBus AeronBus::build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings) {
    AeronBus bus;
    bus.control_source = std::make_unique<MdControlSubscriber>(aeron,
                                                               settings.aeron.md_control.channel,
                                                               settings.aeron.md_control.stream_id);
    bus.md_sink =
        std::make_shared<MdPublisher>(aeron, settings.aeron.md_data.channel, settings.aeron.md_data.stream_id);
    bus.ack_sink =
        std::make_unique<AckPublisher>(aeron, settings.aeron.md_ack_hb.channel, settings.aeron.md_ack_hb.stream_id);
    bus.funding_sink = std::make_shared<FundingRatePublisher>(aeron,
                                                              settings.aeron.funding_rate.channel,
                                                              settings.aeron.funding_rate.stream_id);
    bus.stats_sink = std::make_shared<InstrumentStatsPublisher>(aeron,
                                                          settings.aeron.instrument_stats.channel,
                                                          settings.aeron.instrument_stats.stream_id);
    return bus;
}

}  // namespace bpt::md_gateway::messaging
