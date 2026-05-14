#pragma once

/// \file
/// \brief Bus boundary for bpt-radar.
///
/// Mirror of bpt-analytics: every Aeron pub/sub the service needs is built in
/// one factory so `RadarService` doesn't depend on `<Aeron.h>` directly.

#include "radar/messaging/instrument_stats_subscriber.h"
#include "radar/messaging/market_color_publisher.h"
#include "radar/messaging/vol_surface_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::radar {
namespace config {
struct Settings;
}

namespace messaging {

struct RadarBus {
    std::unique_ptr<VolSurfaceSubscriber> surface_sub;
    std::unique_ptr<InstrumentStatsSubscriber> stats_sub;
    std::unique_ptr<MarketColorPublisher> color_pub;
};

class RadarAeronBus {
public:
    static RadarBus build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::radar
