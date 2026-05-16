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
#include "bridge/messaging/publishers/dashboard_control_publisher.h"
#include "bridge/messaging/subscribers/account_subscriber.h"
#include "bridge/messaging/subscribers/exec_subscriber.h"
#include "bridge/messaging/subscribers/market_color_subscriber.h"
#include "bridge/messaging/subscribers/md_subscriber.h"
#include "bridge/messaging/subscribers/portfolio_snapshot_subscriber.h"
#include "bridge/messaging/subscribers/toxicity_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::bridge::messaging {

struct BridgeBus {
    std::unique_ptr<MdSubscriber> md_sub;
    std::unique_ptr<ExecSubscriber> exec_sub;
    std::unique_ptr<AccountSubscriber> account_sub;
    std::unique_ptr<PortfolioSnapshotSubscriber> portfolio_sub;  ///< optional (null if stream_id == 0)
    std::unique_ptr<ToxicitySubscriber> tox_sub;                 ///< optional
    std::unique_ptr<MarketColorSubscriber> color_sub;            ///< optional
    /// Shared rather than unique so main can hand a `shared_ptr<IDashboardControlSink>`
    /// view of the same object to BridgeService while the bus still owns it.
    std::shared_ptr<DashboardControlPublisher> ctrl_pub;         ///< optional
};

class BridgeAeronBus {
public:
    static BridgeBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace bpt::bridge::messaging
