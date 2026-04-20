#pragma once

#include "strategy/app/startup_gate.h"
#include "strategy/backtest/backtest_client.h"
#include "strategy/config/config.h"
#include "strategy/dashboard/portfolio_snapshot_publisher.h"
#include "strategy/md/md_client.h"
#include "strategy/metrics/metrics.h"
#include "strategy/order/i_order_gateway_client.h"

namespace bpt::strategy::order {
class PaperOrderGatewayClient;  // fwd decl — full include in strategy_app.cpp
}  // namespace bpt::strategy::order
#include "strategy/order/order_manager.h"
#include "strategy/refdata/fee_cache.h"
#include "strategy/refdata/funding_rate_cache.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/vol/vol_surface_client.h"

#include <Aeron.h>
#include <Subscription.h>

#include <cstdint>
#include <memory>
#include <bpt_app/app.h>
#include <bpt_common/util/latency_histogram.h>
#include <bpt_common/util/topology.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::strategy {

class StrategyApp : public bpt::app::IService {
public:
    StrategyApp(config::AppConfig cfg,
                std::shared_ptr<aeron::Aeron> aeron,
                const bpt::common::util::Topology& topology);
    void run() override;
    void stop() override;

private:
    void wire_refdata_callbacks();
    void wire_md_callbacks();
    void wire_vol_callbacks();
    void wire_order_callbacks();
    void run_backtest_loop();
    void check_service_liveness();
    void report_latency_stats();
    void shutdown_flatten();

    config::AppConfig cfg_;
    std::shared_ptr<aeron::Aeron> aeron_;
    metrics::StrategyMetrics metrics_;
    refdata::FeeCache fee_cache_;
    refdata::FundingRateCache funding_rate_cache_;
    std::unique_ptr<refdata::RefdataClient> refdata_;
    std::unique_ptr<md::MdClient> md_client_;
    std::unique_ptr<order::IOrderGatewayClient> order_gw_;

    // Non-owning alias to the order_gw_ above when paper_mode is set —
    // gives wire_md_callbacks a typed handle so it can feed BBO / trade
    // ticks into the fill engine without downcasting the interface.
    // Null otherwise.
    order::PaperOrderGatewayClient* paper_gw_{nullptr};
    std::unique_ptr<vol::VolSurfaceClient> vol_client_;
    std::unique_ptr<order::OrderManager> order_mgr_;
    std::unique_ptr<strategy::IStrategy> strategy_;
    std::unique_ptr<backtest::BacktestClient> backtest_client_;
    std::shared_ptr<aeron::Subscription> tyr_sub_;          // optional: Analytics toxicity stream
    std::shared_ptr<aeron::Subscription> dashboard_ctrl_sub_;
    std::unique_ptr<dashboard::PortfolioSnapshotPublisher> portfolio_snap_pub_;

    // Drives the refdata→accounts→subscribe→snapshot startup sequence.
    // Constructed after strategy_/order_gw_/refdata_ exist.
    std::unique_ptr<app::StartupGate> startup_gate_;

    bool pricer_ready_{false};

    // Service liveness watchdog.
    // Set to true when any service heartbeat goes stale; cleared on recovery.
    // MD and order-gateway callbacks are suppressed while paused.
    bool trading_paused_{false};
    // Set by the dashboard kill-switch via the control stream (9003).
    // Distinct from trading_paused_ so auto-recovery doesn't override
    // a manual halt.
    bool trading_halted_{false};
    uint64_t last_md_hb_recv_ns_{0};  // steady_clock receipt time of last MD Gateway heartbeat
    uint64_t last_gw_hb_recv_ns_{0};  // steady_clock receipt time of last OrderGateway heartbeat
    uint64_t last_liveness_check_ns_{0};

    // Latency histograms — T0 = bpt-md-gateway receipt timestamp in MD message (TSC ns).
    // tick_lat: every MD tick, T0 → strategy callback returns.
    // order_lat: only ticks that result in a placed order, T0 → place_order returns.
    bpt::common::util::LatencyHistogram tick_lat_hist_;
    bpt::common::util::LatencyHistogram order_lat_hist_;
    uint64_t curr_tick_ts_ns_{0};     // T0 of the tick currently being processed
    uint64_t last_lat_report_ns_{0};  // TSC ns of the last latency report

    const bpt::common::util::Topology& topology_;
};

}  // namespace bpt::strategy
