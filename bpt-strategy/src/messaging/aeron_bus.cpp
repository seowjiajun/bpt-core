#include "strategy/messaging/aeron_bus.h"

#include "strategy/app/strategy_service.h"
#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/messaging/subscribers/aeron/console_control_subscriber.h"
#include "strategy/messaging/subscribers/aeron/toxicity_subscriber.h"
#include "strategy/order/aeron_order_gateway_client.h"
#include "strategy/refdata/refdata_client.h"

#include <bpt_common/logging.h>

namespace bpt::strategy::messaging {

StrategyBus StrategyAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::AppConfig& cfg) {
    const auto& ac = cfg.aeron;
    const auto& fc = cfg.strat;

    StrategyBus bus;

    bus.refdata =
        std::make_unique<refdata::AeronRefdataClient<StrategyService>>(aeron,
                                                                       ac.refdata,
                                                                       fc.strategy.schedule.max_refdata_staleness_ns);

    if (ac.md.control.stream_id != 0) {
        bus.md = std::make_unique<md::AeronMdClient<StrategyService>>(aeron, ac.md);
    }

    if (ac.order.submit.stream_id != 0) {
        bus.order_gw = std::make_unique<order::AeronOrderGatewayClient<StrategyService>>(aeron, ac.order);
    }

    if (ac.vol.surface.stream_id != 0) {
        bus.vol = std::make_unique<vol::VolSurfaceClient<StrategyService>>(aeron, ac.vol);
    }

    if (ac.toxicity.stream_id != 0) {
        bus.tox = std::make_unique<aeron::ToxicitySubscriber<StrategyService>>(aeron, ac.toxicity);
    }

    if (cfg.backtest_mode) {
        bus.backtest = std::make_unique<backtest::BacktestClient>(aeron, ac.backtest);
    }

    // Console control + snapshot — disabled in backtest mode (backtest
    // has its own control channel; portfolio snapshots are noise during
    // replay).
    if (!cfg.backtest_mode && ac.console_control.stream_id != 0) {
        bus.console_ctrl =
            std::make_unique<aeron::ConsoleControlSubscriber<StrategyService>>(aeron, ac.console_control);
        if (bus.console_ctrl->is_ready()) {
            bpt::common::log::info("Console control subscription ready on stream {}", ac.console_control.stream_id);
        } else {
            bpt::common::log::warn("Console control subscription unavailable");
        }
    }

    if (!cfg.backtest_mode) {
        bus.portfolio_snap = std::make_unique<console::PortfolioSnapshotPublisher>(aeron, ac.portfolio);
    }

    return bus;
}

}  // namespace bpt::strategy::messaging
