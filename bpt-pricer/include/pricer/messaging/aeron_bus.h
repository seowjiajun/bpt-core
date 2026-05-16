#pragma once

/// @file
/// Bus boundary for bpt-pricer. Mirrors the shape used by bpt-refdata,
/// bpt-md-gateway, bpt-order-gateway, and bpt-strategy: every concrete
/// Aeron pub/sub the pricer needs is constructed in one factory so
/// `PricerService` doesn't have to take `<Aeron.h>` in its constructor.
///
/// VolSurfacePublisher is promoted to a port (IVolSurfacePublisher,
/// concrete AeronVolSurfacePublisher) so the deterministic backtester
/// can substitute an InProcessVolSurfacePublisher that bypasses SBE
/// encode + Aeron offer entirely. Off-hot-path vtable cost is invisible
/// at the ~Hz cadence of vol-surface rebuilds. The remaining publishers
/// + subscribers stay as concrete classes — promote each individually
/// when a non-Aeron consumer materialises (same pragmatic call as
/// bpt-strategy / bpt-analytics).

#include "pricer/md/md_subscribe_client.h"
#include "pricer/md/md_subscriber.h"
#include "pricer/messaging/publishers/i_vol_surface_publisher.h"
#include "pricer/messaging/publishers/status_publisher.h"
#include "pricer/refdata/refdata_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::pricer {
namespace config {
struct Settings;
}

namespace messaging {

struct PricerBus {
    std::unique_ptr<IVolSurfacePublisher> vol_pub;  ///< port; AeronVolSurfacePublisher in prod
    std::unique_ptr<StatusPublisher> status_pub;
    std::unique_ptr<md::MdSubscriber> md_sub;
    std::unique_ptr<md::MdSubscribeClient> md_ctrl;  ///< pricer → md-gateway: subscribe batches
    std::unique_ptr<refdata::RefdataSubscriber> refdata_sub;
};

class PricerAeronBus {
public:
    /// Build every Aeron-touching object the pricer needs. Sole place
    /// that calls into `<Aeron.h>` from the application layer.
    static PricerBus build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::pricer
