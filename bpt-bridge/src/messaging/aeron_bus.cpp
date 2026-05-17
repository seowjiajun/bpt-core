#include "bridge/messaging/aeron_bus.h"

#include "bridge/messaging/subscribers/aeron/account_subscriber.h"
#include "bridge/messaging/subscribers/aeron/exec_subscriber.h"
#include "bridge/messaging/subscribers/aeron/market_color_subscriber.h"
#include "bridge/messaging/subscribers/aeron/md_subscriber.h"
#include "bridge/messaging/subscribers/aeron/portfolio_snapshot_subscriber.h"
#include "bridge/messaging/subscribers/aeron/toxicity_subscriber.h"

namespace bpt::bridge::messaging {

BridgeBus BridgeAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    BridgeBus bus;

    // Always-on streams
    bus.md_sub = std::make_unique<aeron::MdSubscriber>(aeron, settings.md_data.channel, settings.md_data.stream_id);
    bus.exec_sub =
        std::make_unique<aeron::ExecSubscriber>(aeron, settings.exec_report.channel, settings.exec_report.stream_id);
    bus.account_sub = std::make_unique<aeron::AccountSubscriber>(aeron,
                                                                 settings.account_snapshot.channel,
                                                                 settings.account_snapshot.stream_id);

    // Optional streams — stream_id == 0 means "not configured, skip".
    if (settings.portfolio.stream_id != 0) {
        bus.portfolio_sub = std::make_unique<aeron::PortfolioSnapshotSubscriber>(aeron,
                                                                                 settings.portfolio.channel,
                                                                                 settings.portfolio.stream_id);
    }
    if (settings.toxicity.stream_id != 0) {
        bus.tox_sub =
            std::make_unique<aeron::ToxicitySubscriber>(aeron, settings.toxicity.channel, settings.toxicity.stream_id);
    }
    if (settings.market_color.stream_id != 0) {
        bus.color_sub = std::make_unique<aeron::MarketColorSubscriber>(aeron,
                                                                       settings.market_color.channel,
                                                                       settings.market_color.stream_id);
    }
    if (settings.dashboard_control.stream_id != 0) {
        bus.ctrl_pub = std::make_shared<aeron::DashboardControlPublisher>(aeron,
                                                                          settings.dashboard_control.channel,
                                                                          settings.dashboard_control.stream_id);
    }

    return bus;
}

}  // namespace bpt::bridge::messaging
