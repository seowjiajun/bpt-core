#include "pricer/messaging/aeron_bus.h"

#include "pricer/app/pricer_service.h"
#include "pricer/config/settings.h"
#include "pricer/md/aeron/md_subscribe_client.h"
#include "pricer/messaging/publishers/aeron/status_publisher.h"
#include "pricer/messaging/publishers/aeron/vol_surface_publisher.h"

namespace bpt::pricer::messaging {

PricerBus PricerAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    PricerBus bus;
    bus.vol_pub = std::make_unique<aeron::VolSurfacePublisher>(aeron,
                                                               settings.vol_surface.channel,
                                                               settings.vol_surface.stream_id);
    bus.status_pub = std::make_unique<aeron::StatusPublisher>(aeron,
                                                              settings.pricer_status.channel,
                                                              settings.pricer_status.stream_id);
    bus.md_sub = std::make_unique<md::aeron::MdSubscriber<PricerService>>(
        aeron, settings.md_data.channel, settings.md_data.stream_id);
    bus.md_ctrl = std::make_unique<md::aeron::MdSubscribeClient>(aeron,
                                                                 settings.md_control.channel,
                                                                 settings.md_control.stream_id);
    bus.refdata_sub = std::make_unique<refdata::aeron::RefdataSubscriber<PricerService>>(
        aeron,
        settings.refdata_snapshot.channel,
        settings.refdata_snapshot.stream_id,
        settings.refdata_delta.channel,
        settings.refdata_delta.stream_id,
        settings.refdata_control.channel,
        settings.refdata_control.stream_id);
    return bus;
}

}  // namespace bpt::pricer::messaging
