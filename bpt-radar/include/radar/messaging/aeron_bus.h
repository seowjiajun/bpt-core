#pragma once

/// \file
/// \brief Bus boundary for bpt-radar.
///
/// Mirror of bpt-analytics: every Aeron pub/sub the service needs is built in
/// one factory so `RadarService` doesn't depend on `<Aeron.h>` directly.

#include "radar/messaging/publishers/api/market_color_publisher.h"
#include "radar/messaging/subscribers/aeron/funding_rate_subscriber.h"
#include "radar/messaging/subscribers/aeron/instrument_stats_subscriber.h"
#include "radar/messaging/subscribers/aeron/md_market_data_subscriber.h"
#include "radar/messaging/subscribers/aeron/md_trade_subscriber.h"
#include "radar/messaging/subscribers/aeron/refdata_perp_subscriber.h"
#include "radar/messaging/subscribers/aeron/vol_surface_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::radar {
class RadarService;
namespace config {
struct Settings;
}

namespace messaging {

/// Each subscriber is the concrete CRTP-templated instantiation on
/// RadarService. Templated dispatch directly into RadarService::on_*
/// — zero std::function indirection per fragment.
struct RadarBus {
    std::unique_ptr<aeron::VolSurfaceSubscriber<RadarService>> surface_sub;
    std::unique_ptr<aeron::InstrumentStatsSubscriber<RadarService>> stats_sub;
    std::unique_ptr<aeron::FundingRateSubscriber<RadarService>> funding_sub;
    std::unique_ptr<aeron::RefdataPerpSubscriber<RadarService>> refdata_perp_sub;
    std::unique_ptr<aeron::MdTradeSubscriber<RadarService>> trade_sub;
    std::unique_ptr<aeron::MdMarketDataSubscriber<RadarService>> bbo_sub;
    std::unique_ptr<api::MarketColorPublisher> color_pub;
};

class RadarAeronBus {
public:
    static RadarBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::radar
