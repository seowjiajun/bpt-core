#pragma once

/// @file
/// Bus boundary for bpt-bridge. Every Aeron pub/sub the service needs is
/// built in one factory so BridgeService doesn't depend on `<Aeron.h>`
/// directly — matches the shape used by all the other Aeron-coupled
/// services (analytics, md-gateway, order-gateway, pricer, radar, refdata,
/// strategy, pms).
///
/// Streams marked "optional" use a stream_id == 0 sentinel in the config
/// to mean "not subscribed" — the factory skips them and leaves the
/// unique_ptr null.

#include "bridge/config/settings.h"
#include "bridge/messaging/publishers/aeron/dashboard_control_publisher.h"
#include "bridge/messaging/subscribers/api/account_subscriber.h"
#include "bridge/messaging/subscribers/api/exec_subscriber.h"
#include "bridge/messaging/subscribers/api/market_color_subscriber.h"
#include "bridge/messaging/subscribers/api/md_subscriber.h"
#include "bridge/messaging/subscribers/api/portfolio_snapshot_subscriber.h"
#include "bridge/messaging/subscribers/api/toxicity_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::bridge::messaging {

struct BridgeBus {
    std::unique_ptr<api::MdSubscriber> md_sub;                       ///< port
    std::unique_ptr<api::ExecSubscriber> exec_sub;                   ///< port
    std::unique_ptr<api::AccountSubscriber> account_sub;             ///< port
    std::unique_ptr<api::PortfolioSnapshotSubscriber> portfolio_sub; ///< optional port (null if stream_id == 0)
    std::unique_ptr<api::ToxicitySubscriber> tox_sub;                ///< optional port
    std::unique_ptr<api::MarketColorSubscriber> color_sub;           ///< optional port
    /// Shared rather than unique so main can hand a `shared_ptr<api::DashboardControlPublisher>`
    /// view of the same object to BridgeService while the bus still owns it.
    std::shared_ptr<aeron::DashboardControlPublisher> ctrl_pub;         ///< optional
};

class BridgeAeronBus {
public:
    static BridgeBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace bpt::bridge::messaging
