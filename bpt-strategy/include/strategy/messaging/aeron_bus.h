#pragma once

/// @file
/// Bus boundary for bpt-strategy. Mirrors the shape used by bpt-refdata,
/// bpt-md-gateway, and bpt-order-gateway: every concrete Aeron pub/sub
/// the strategy needs is constructed in one factory so `StrategyService`
/// doesn't have to include `<Aeron.h>` itself.
///
/// Why this isn't a list of formal port interfaces (yet): strategy's
/// existing client classes (RefdataClient, MdClient, VolSurfaceClient,
/// BacktestClient, PortfolioSnapshotPublisher) already encapsulate
/// their pub/sub plumbing behind concrete classes with std::function
/// callbacks. Promoting each to its own port adds vtable on poll() —
/// fine off the hot path — but offers no immediate test-seam payoff
/// the codebase needs today. Defer until a fake-bus test actually
/// blocks on the abstraction. `IOrderGatewayClient` already exists for
/// historical reasons (used to have a paper impl alongside) so it is
/// kept and bus output is typed by the interface.

#include "strategy/backtest/backtest_client.h"
#include "strategy/dashboard/portfolio_snapshot_publisher.h"
#include "strategy/md/i_md_client.h"
#include "strategy/messaging/dashboard_control_subscriber.h"
#include "strategy/messaging/toxicity_subscriber.h"
#include "strategy/order/i_order_gateway_client.h"
#include "strategy/refdata/i_refdata_client.h"
#include "strategy/vol/vol_surface_client.h"

#include <Aeron.h>

#include <memory>

namespace bpt::strategy {
namespace config {
struct AppConfig;
}

namespace messaging {

/// Bundle of every transport-coupled object the strategy needs. Optional
/// fields are null when the corresponding stream_id in config is 0
/// (matches the existing per-feature opt-in shape).
///
/// `refdata`, `md`, and `order_gw` are typed against polymorphic interfaces
/// so a deterministic backtest harness can substitute in-process
/// implementations without any change to the strategy. The remaining
/// fields are still typed against concrete classes — they're either
/// off-hot-path (vol, dashboard) or only used in production paths
/// (backtest, tox); promoting them to interfaces is deferred until a
/// concrete consumer needs the substitution.
struct StrategyBus {
    std::unique_ptr<refdata::IRefdataClient> refdata;
    std::unique_ptr<md::IMdClient> md;
    std::unique_ptr<order::IOrderGatewayClient> order_gw;
    std::unique_ptr<vol::VolSurfaceClient> vol;
    std::unique_ptr<backtest::BacktestClient> backtest;
    std::unique_ptr<ToxicitySubscriber> tox;
    std::unique_ptr<DashboardControlSubscriber> dashboard_ctrl;
    std::unique_ptr<dashboard::PortfolioSnapshotPublisher> portfolio_snap;
};

class StrategyAeronBus {
public:
    /// Build every concrete pub/sub the strategy needs from a single
    /// Aeron client + the parsed config. Sole place that calls into
    /// `<Aeron.h>` from the application layer.
    static StrategyBus build(std::shared_ptr<aeron::Aeron> aeron, const config::AppConfig& cfg);
};

}  // namespace messaging
}  // namespace bpt::strategy
