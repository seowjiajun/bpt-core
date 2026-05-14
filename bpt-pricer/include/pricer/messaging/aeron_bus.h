#pragma once

/// @file
/// Bus boundary for bpt-pricer. Mirrors the shape used by bpt-refdata,
/// bpt-md-gateway, bpt-order-gateway, and bpt-strategy: every concrete
/// Aeron pub/sub the pricer needs is constructed in one factory so
/// `PricerService` doesn't have to take `<Aeron.h>` in its constructor.
///
/// Pricer's existing concrete classes (VolSurfacePublisher,
/// StatusPublisher, MdSubscriber, RefdataSubscriber) already encapsulate
/// their Aeron plumbing behind narrow domain APIs. Promoting each to a
/// formal port adds vtable on poll() — fine off the hot path — but
/// offers no test-seam payoff today. Defer until a fake-bus test
/// actually blocks. Same pragmatic call as bpt-strategy.

#include "pricer/md/md_subscribe_client.h"
#include "pricer/md/md_subscriber.h"
#include "pricer/messaging/status_publisher.h"
#include "pricer/messaging/vol_surface_publisher.h"
#include "pricer/refdata/refdata_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::pricer {
namespace config {
struct Settings;
}

namespace messaging {

struct PricerBus {
    std::unique_ptr<VolSurfacePublisher> vol_pub;
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
