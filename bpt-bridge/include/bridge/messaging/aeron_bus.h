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
#include "bridge/messaging/publishers/aeron/console_control_publisher.h"
#include "bridge/messaging/subscribers/aeron/account_subscriber.h"
#include "bridge/messaging/subscribers/aeron/exec_subscriber.h"
#include "bridge/messaging/subscribers/aeron/market_color_subscriber.h"
#include "bridge/messaging/subscribers/aeron/md_subscriber.h"
#include "bridge/messaging/subscribers/aeron/portfolio_snapshot_subscriber.h"
#include "bridge/messaging/subscribers/aeron/toxicity_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::bridge {
class BridgeService;
}

namespace bpt::bridge::messaging {

/// Each subscriber is the concrete CRTP-templated instantiation on
/// BridgeService. Templated dispatch directly into BridgeService::on_*
/// — zero std::function indirection per fragment.
struct BridgeBus {
    std::unique_ptr<aeron::MdSubscriber<BridgeService>> md_sub;
    std::unique_ptr<aeron::ExecSubscriber<BridgeService>> exec_sub;
    std::unique_ptr<aeron::AccountSubscriber<BridgeService>> account_sub;
    std::unique_ptr<aeron::PortfolioSnapshotSubscriber<BridgeService>> portfolio_sub;
    std::unique_ptr<aeron::ToxicitySubscriber<BridgeService>> tox_sub;
    std::unique_ptr<aeron::MarketColorSubscriber<BridgeService>> color_sub;
    /// Shared rather than unique so main can hand a `shared_ptr<api::ConsoleControlPublisher>`
    /// view of the same object to BridgeService while the bus still owns it.
    std::shared_ptr<aeron::ConsoleControlPublisher> ctrl_pub;  ///< optional
};

class BridgeAeronBus {
public:
    static BridgeBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace bpt::bridge::messaging
