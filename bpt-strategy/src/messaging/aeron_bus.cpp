#include "strategy/messaging/aeron_bus.h"

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/aeron_order_gateway_client.h"
#include "strategy/refdata/refdata_client.h"

#include <bpt_common/logging.h>

namespace bpt::strategy::messaging {

StrategyBus StrategyAeronBus::build(std::shared_ptr<aeron::Aeron> aeron, const config::AppConfig& cfg) {
    const auto& ac = cfg.aeron;
    const auto& fc = cfg.strat;

    StrategyBus bus;

    bus.refdata = std::make_unique<refdata::AeronRefdataClient>(aeron,
                                                                ac.refdata_control.channel,
                                                                ac.refdata_control.stream_id,
                                                                ac.refdata_snapshot.stream_id,
                                                                ac.refdata_delta.stream_id,
                                                                ac.fee_schedule.stream_id,
                                                                ac.funding_rate.stream_id,
                                                                ac.refdata_status.stream_id,
                                                                fc.strategy.schedule.max_refdata_staleness_ns);

    if (ac.md_control.stream_id != 0) {
        bus.md = std::make_unique<md::AeronMdClient>(aeron,
                                                     ac.md_control.channel,
                                                     ac.md_control.stream_id,
                                                     ac.md_data.stream_id,
                                                     ac.md_ack_hb.stream_id);
    }

    if (ac.order.stream_id != 0) {
        bus.order_gw = std::make_unique<order::AeronOrderGatewayClient>(aeron,
                                                                        ac.order.channel,
                                                                        ac.order.stream_id,
                                                                        ac.exec_report.stream_id,
                                                                        ac.heartbeat.stream_id,
                                                                        ac.account_snapshot.stream_id);
    }

    if (ac.vol_surface.stream_id != 0) {
        bus.vol = std::make_unique<vol::VolSurfaceClient>(aeron,
                                                          ac.vol_surface.channel,
                                                          ac.vol_surface.stream_id,
                                                          ac.pricer_status.stream_id);
        bpt::common::log::info("VolSurfaceClient ready: surface={} status={}",
                               ac.vol_surface.stream_id,
                               ac.pricer_status.stream_id);
    }

    if (ac.toxicity.stream_id != 0) {
        bus.tox = std::make_unique<ToxicitySubscriber>(aeron, ac.toxicity.channel, ac.toxicity.stream_id);
        bpt::common::log::info("Analytics toxicity subscription ready: {} stream {}",
                               ac.toxicity.channel,
                               ac.toxicity.stream_id);
    }

    if (cfg.backtest_mode) {
        bus.backtest =
            std::make_unique<backtest::BacktestClient>(aeron,
                                                       ac.backtest_control.channel,
                                                       ac.backtest_control.stream_id,  // sub: Backtester → Strategy
                                                       ac.backtest_ack.stream_id);     // pub: Strategy → Backtester
        bpt::common::log::info("Backtest mode enabled: ctrl_sub={} ack_pub={}",
                               ac.backtest_control.stream_id,
                               ac.backtest_ack.stream_id);
    }

    // Dashboard control + snapshot — disabled in backtest mode (backtest
    // has its own control channel; portfolio snapshots are noise during
    // replay).
    if (!cfg.backtest_mode && ac.dashboard_control.stream_id != 0) {
        bus.dashboard_ctrl = std::make_unique<DashboardControlSubscriber>(aeron,
                                                                          ac.dashboard_control.channel,
                                                                          ac.dashboard_control.stream_id);
        if (bus.dashboard_ctrl->is_ready()) {
            bpt::common::log::info("Dashboard control subscription ready on stream {}", ac.dashboard_control.stream_id);
        } else {
            bpt::common::log::warn("Dashboard control subscription unavailable");
        }
    }

    if (!cfg.backtest_mode) {
        bus.portfolio_snap = std::make_unique<dashboard::PortfolioSnapshotPublisher>(aeron,
                                                                                     ac.dashboard_snapshot.channel,
                                                                                     ac.dashboard_snapshot.stream_id);
    }

    return bus;
}

}  // namespace bpt::strategy::messaging
