#include "pricer/messaging/aeron_bus.h"

#include "pricer/config/settings.h"
#include "pricer/messaging/publishers/aeron_vol_surface_publisher.h"

namespace bpt::pricer::messaging {

PricerBus PricerAeronBus::build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings) {
    PricerBus bus;
    bus.vol_pub = std::make_unique<AeronVolSurfacePublisher>(aeron,
                                                             settings.vol_surface.channel,
                                                             settings.vol_surface.stream_id);
    bus.status_pub =
        std::make_unique<StatusPublisher>(aeron, settings.pricer_status.channel, settings.pricer_status.stream_id);
    bus.md_sub = std::make_unique<md::MdSubscriber>(aeron, settings.md_data.channel, settings.md_data.stream_id);
    bus.md_ctrl =
        std::make_unique<md::MdSubscribeClient>(aeron, settings.md_control.channel, settings.md_control.stream_id);
    bus.refdata_sub = std::make_unique<refdata::RefdataSubscriber>(aeron,
                                                                   settings.refdata_snapshot.channel,
                                                                   settings.refdata_snapshot.stream_id,
                                                                   settings.refdata_delta.channel,
                                                                   settings.refdata_delta.stream_id,
                                                                   settings.refdata_control.channel,
                                                                   settings.refdata_control.stream_id);
    return bus;
}

}  // namespace bpt::pricer::messaging
