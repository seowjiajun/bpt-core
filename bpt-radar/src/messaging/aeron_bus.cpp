#include "radar/messaging/aeron_bus.h"

#include "radar/config/settings.h"

namespace bpt::radar::messaging {

RadarBus RadarAeronBus::build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings) {
    RadarBus bus;
    bus.surface_sub =
        std::make_unique<VolSurfaceSubscriber>(aeron, settings.vol_surface.channel, settings.vol_surface.stream_id);
    bus.stats_sub = std::make_unique<InstrumentStatsSubscriber>(aeron,
                                                                settings.instrument_stats.channel,
                                                                settings.instrument_stats.stream_id);
    bus.color_pub =
        std::make_unique<MarketColorPublisher>(aeron, settings.market_color.channel, settings.market_color.stream_id);
    return bus;
}

}  // namespace bpt::radar::messaging
