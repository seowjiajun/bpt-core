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
    bus.funding_sub = std::make_unique<FundingRateSubscriber>(aeron,
                                                              settings.funding_rate.channel,
                                                              settings.funding_rate.stream_id);
    bus.refdata_perp_sub = std::make_unique<RefdataPerpSubscriber>(aeron,
                                                                   settings.refdata_snapshot.channel,
                                                                   settings.refdata_snapshot.stream_id);
    bus.trade_sub =
        std::make_unique<MdTradeSubscriber>(aeron, settings.md_data.channel, settings.md_data.stream_id);
    bus.bbo_sub = std::make_unique<MdMarketDataSubscriber>(aeron,
                                                           settings.md_data.channel,
                                                           settings.md_data.stream_id);
    bus.color_pub =
        std::make_unique<MarketColorPublisher>(aeron, settings.market_color.channel, settings.market_color.stream_id);
    return bus;
}

}  // namespace bpt::radar::messaging
