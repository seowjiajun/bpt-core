#include "bridge/messaging/aeron_bus.h"

#include "bridge/app/bridge_service.h"

namespace bpt::bridge::messaging {

BridgeBus BridgeAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    BridgeBus bus;

    // Always-on streams
    bus.md_sub = std::make_unique<aeron::MdSubscriber<BridgeService>>(aeron, settings.md_data);
    bus.exec_sub = std::make_unique<aeron::ExecSubscriber<BridgeService>>(aeron, settings.exec_report);
    bus.account_sub = std::make_unique<aeron::AccountSubscriber<BridgeService>>(aeron, settings.account_snapshot);

    // Optional streams — stream_id == 0 means "not configured, skip".
    if (settings.portfolio.stream_id != 0) {
        bus.portfolio_sub =
            std::make_unique<aeron::PortfolioSnapshotSubscriber<BridgeService>>(aeron, settings.portfolio);
    }
    if (settings.toxicity.stream_id != 0) {
        bus.tox_sub = std::make_unique<aeron::ToxicitySubscriber<BridgeService>>(aeron, settings.toxicity);
    }
    if (settings.market_color.stream_id != 0) {
        bus.color_sub = std::make_unique<aeron::MarketColorSubscriber<BridgeService>>(aeron, settings.market_color);
    }
    if (settings.console_control.stream_id != 0) {
        bus.ctrl_pub = std::make_shared<aeron::ConsoleControlPublisher>(aeron, settings.console_control);
    }

    return bus;
}

}  // namespace bpt::bridge::messaging
