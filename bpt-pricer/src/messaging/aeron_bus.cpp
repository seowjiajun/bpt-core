#include "pricer/messaging/aeron_bus.h"

#include "pricer/app/pricer_service.h"
#include "pricer/config/settings.h"
#include "pricer/md/aeron/md_subscribe_client.h"
#include "pricer/messaging/publishers/aeron/status_publisher.h"
#include "pricer/messaging/publishers/aeron/vol_surface_publisher.h"

namespace bpt::pricer::messaging {

PricerBus PricerAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    const auto& ac = settings.aeron;

    PricerBus bus;
    bus.vol_pub = std::make_unique<aeron::VolSurfacePublisher>(aeron, ac.vol_surface);
    bus.status_pub = std::make_unique<aeron::StatusPublisher>(aeron, ac.pricer_status);
    bus.md_sub = std::make_unique<md::aeron::MdSubscriber<PricerService>>(aeron, ac.md.data);
    bus.md_ctrl = std::make_unique<md::aeron::MdSubscribeClient>(aeron, ac.md.control);
    bus.refdata_sub = std::make_unique<refdata::aeron::RefdataSubscriber<PricerService>>(aeron, ac.refdata);
    return bus;
}

}  // namespace bpt::pricer::messaging
