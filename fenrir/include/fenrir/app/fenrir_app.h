#pragma once

#include "fenrir/app/startup_gate.h"
#include "fenrir/backtest/backtest_client.h"
#include "fenrir/config/config.h"
#include "fenrir/dashboard/portfolio_snapshot_publisher.h"
#include "fenrir/md/md_client.h"
#include "fenrir/metrics/metrics.h"
#include "fenrir/order/order_gateway_client.h"
#include "fenrir/order/order_manager.h"
#include "fenrir/refdata/fee_cache.h"
#include "fenrir/refdata/funding_rate_cache.h"
#include "fenrir/refdata/refdata_client.h"
#include "fenrir/strategy/i_strategy.h"
#include "fenrir/vol/vol_surface_client.h"

#include <Aeron.h>
#include <Subscription.h>

#include <cstdint>
#include <memory>
#include <yggdrasil/util/latency_histogram.h>
#include <yggdrasil/util/tsc_clock.h>

namespace fenrir {

class FenrirApp {
public:
    FenrirApp(config::AppConfig cfg, std::shared_ptr<aeron::Aeron> aeron);
    void run();

private:
    void wire_refdata_callbacks();
    void wire_md_callbacks();
    void wire_vol_callbacks();
    void wire_order_callbacks();
    void run_backtest_loop();
    void check_service_liveness();
    void report_latency_stats();

    config::AppConfig cfg_;
    std::shared_ptr<aeron::Aeron> aeron_;
    metrics::FenrirMetrics metrics_;
    refdata::FeeCache fee_cache_;
    refdata::FundingRateCache funding_rate_cache_;
    std::unique_ptr<refdata::RefdataClient> refdata_;
    std::unique_ptr<md::MdClient> md_client_;
    std::unique_ptr<order::OrderGatewayClient> order_gw_;
    std::unique_ptr<vol::VolSurfaceClient> vol_client_;
    std::unique_ptr<order::OrderManager> order_mgr_;
    std::unique_ptr<strategy::IStrategy> strategy_;
    std::unique_ptr<backtest::BacktestClient> backtest_client_;
    std::shared_ptr<aeron::Subscription> dashboard_ctrl_sub_;
    std::unique_ptr<dashboard::PortfolioSnapshotPublisher> portfolio_snap_pub_;

    // Drives the refdata→accounts→subscribe→snapshot startup sequence.
    // Constructed after strategy_/order_gw_/refdata_ exist.
    std::unique_ptr<app::StartupGate> startup_gate_;

    bool surtr_ready_{false};

    // Service liveness watchdog.
    // Set to true when any service heartbeat goes stale; cleared on recovery.
    // MD and order-gateway callbacks are suppressed while paused.
    bool trading_paused_{false};
    // Set by the dashboard kill-switch via the control stream (9003).
    // Distinct from trading_paused_ so auto-recovery doesn't override
    // a manual halt.
    bool trading_halted_{false};
    uint64_t last_md_hb_recv_ns_{0};  // steady_clock receipt time of last Huginn heartbeat
    uint64_t last_gw_hb_recv_ns_{0};  // steady_clock receipt time of last Heimdall heartbeat
    uint64_t last_liveness_check_ns_{0};

    // Latency histograms — T0 = huginn receipt timestamp in MD message (TSC ns).
    // tick_lat: every MD tick, T0 → strategy callback returns.
    // order_lat: only ticks that result in a placed order, T0 → place_order returns.
    ygg::util::LatencyHistogram tick_lat_hist_;
    ygg::util::LatencyHistogram order_lat_hist_;
    uint64_t curr_tick_ts_ns_{0};     // T0 of the tick currently being processed
    uint64_t last_lat_report_ns_{0};  // TSC ns of the last latency report
};

}  // namespace fenrir
