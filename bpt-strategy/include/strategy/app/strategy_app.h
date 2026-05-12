#pragma once

#include "strategy/app/startup_gate.h"
#include "strategy/config/config.h"
#include "strategy/messaging/aeron_bus.h"
#include "strategy/metrics/metrics.h"
#include "strategy/order/order_manager.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/refdata_stale_gate.h"

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
                messaging::StrategyBus bus,
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
    void check_refdata_watchdog();
    void report_latency_stats();
    void shutdown_flatten();

    config::AppConfig cfg_;
    metrics::StrategyMetrics metrics_;
    messaging::StrategyBus bus_;
    std::unique_ptr<order::OrderManager> order_mgr_;
    std::unique_ptr<strategy::IStrategy> strategy_;

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

    // Refdata heartbeat tracker. Tells the strategy to pause new quotes
    // when bpt-refdata stops publishing — without this, AS keeps quoting
    // off cached fees that go stale and silently ships zero-fee-buffer
    // trades (item #16, prod hardening backlog).
    strategy::RefdataStaleGate refdata_stale_gate_;
    uint64_t startup_anchor_ns_{0};   // steady_clock @ run() entry — feeds startup timeout
    uint64_t last_refdata_check_ns_{0};  // 1Hz rate limit for check_refdata_watchdog
    bool refdata_stale_logged_{false};   // edge-trigger for the GoneStale WARN log

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
