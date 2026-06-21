#pragma once

/// @file
/// Bus boundary for bpt-pricer. Mirrors the shape used by bpt-refdata,
/// bpt-md-gateway, bpt-order-gateway, and bpt-strategy: every concrete
/// Aeron pub/sub the pricer needs is constructed in one factory so
/// `PricerService` doesn't have to take `<Aeron.h>` in its constructor.
///
/// All five pub/subs are typed as ports (api::*) so a sim variant can
/// substitute any of them. The aeron::* concretes are wired in at the
/// composition root (PricerAeronBus::build). VolSurface already has a
/// sim variant in `messaging/publishers/sim/`; the others can grow one
/// when the deterministic backtester needs them.

#include "pricer/md/aeron/md_subscriber.h"
#include "pricer/md/api/md_subscribe_client.h"
#include "pricer/messaging/publishers/api/status_publisher.h"
#include "pricer/messaging/publishers/api/vol_surface_publisher.h"
#include "pricer/refdata/aeron/refdata_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::pricer {
class PricerService;
namespace config {
struct Settings;
}

namespace messaging {

struct PricerBus {
    std::unique_ptr<api::VolSurfacePublisher> vol_pub;  ///< port; aeron::VolSurfacePublisher in prod
    std::unique_ptr<api::StatusPublisher> status_pub;   ///< port; aeron::StatusPublisher in prod
    std::unique_ptr<md::aeron::MdSubscriber<PricerService>> md_sub;
    std::unique_ptr<md::api::MdSubscribeClient> md_ctrl;  ///< port; pricer → md-gateway subscribe batches
    std::unique_ptr<refdata::aeron::RefdataSubscriber<PricerService>> refdata_sub;
};

class PricerAeronBus {
public:
    /// Build every Aeron-touching object the pricer needs. Sole place
    /// that calls into `<Aeron.h>` from the application layer.
    static PricerBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::pricer
