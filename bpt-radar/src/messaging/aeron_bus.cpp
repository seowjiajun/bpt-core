#include "radar/messaging/aeron_bus.h"

#include "radar/app/radar_service.h"
#include "radar/config/settings.h"
#include "radar/messaging/publishers/aeron/market_color_publisher.h"

namespace bpt::radar::messaging {

RadarBus RadarAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    RadarBus bus;
    bus.surface_sub = std::make_unique<aeron::VolSurfaceSubscriber<RadarService>>(aeron, settings.vol_surface);
    bus.stats_sub = std::make_unique<aeron::InstrumentStatsSubscriber<RadarService>>(aeron, settings.instrument_stats);
    bus.funding_sub = std::make_unique<aeron::FundingRateSubscriber<RadarService>>(aeron, settings.funding_rate);
    bus.refdata_perp_sub =
        std::make_unique<aeron::RefdataPerpSubscriber<RadarService>>(aeron, settings.refdata_snapshot);
    bus.trade_sub = std::make_unique<aeron::MdTradeSubscriber<RadarService>>(aeron, settings.md_data);
    bus.bbo_sub = std::make_unique<aeron::MdMarketDataSubscriber<RadarService>>(aeron, settings.md_data);
    bus.color_pub = std::make_unique<aeron::MarketColorPublisher>(aeron, settings.market_color);
    return bus;
}

}  // namespace bpt::radar::messaging
