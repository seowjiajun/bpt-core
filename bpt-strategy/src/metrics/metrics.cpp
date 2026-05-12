#include "strategy/metrics/metrics.h"

#include <prometheus/histogram.h>
#include <string>

namespace bpt::strategy::metrics {

const prometheus::Histogram::BucketBoundaries& StrategyMetrics::kDefaultLatencyBucketsNs() {
    // Covers 1 µs → 1 s, widest at the slow end where tail-latency events
    // land (GC pauses, scheduler stalls, exchange-induced stop-the-world).
    static const prometheus::Histogram::BucketBoundaries k{
        1e3,
        5e3,
        10e3,
        25e3,
        50e3,
        100e3,
        250e3,
        500e3,
        1e6,
        5e6,
        10e6,
        50e6,
        100e6,
        500e6,
        1e9,
    };
    return k;
}

StrategyMetrics::StrategyMetrics(int port) {
    registry = std::make_shared<prometheus::Registry>();

    exposer = std::make_unique<prometheus::Exposer>("0.0.0.0:" + std::to_string(port));
    exposer->RegisterCollectable(registry);

    auto& h = prometheus::BuildGauge().Name("strategy_healthy").Help("1 if Strategy is running").Register(*registry);
    healthy = &h.Add({});
    healthy->Set(1.0);

    auto& sa = prometheus::BuildGauge()
                   .Name("strategy_strategy_active")
                   .Help("1 if the strategy has started (past the startup gate)")
                   .Register(*registry);
    strategy_active = &sa.Add({});
    strategy_active->Set(0.0);

    auto& tp = prometheus::BuildGauge()
                   .Name("strategy_trading_paused")
                   .Help("1 if trading is paused (service heartbeat stale or kill switch)")
                   .Register(*registry);
    trading_paused = &tp.Add({});
    trading_paused->Set(0.0);

    auto& th = prometheus::BuildGauge()
                   .Name("strategy_trading_halted")
                   .Help("1 if dashboard kill-switch latched — no automatic recovery")
                   .Register(*registry);
    trading_halted = &th.Add({});
    trading_halted->Set(0.0);

    auto& rs = prometheus::BuildGauge()
                   .Name("strategy_refdata_stale")
                   .Help("1 if strategy detected stale refdata heartbeat and paused new quotes")
                   .Register(*registry);
    refdata_stale = &rs.Add({});
    refdata_stale->Set(0.0);

    auto& rd = prometheus::BuildCounter()
                   .Name("strategy_reconciliation_divergences_total")
                   .Help("Count of reconciliation passes that produced at least one divergence")
                   .Register(*registry);
    reconciliation_divergences_total = &rd.Add({});

    account_snapshot_last_recv_ns_fam =
        &prometheus::BuildGauge()
             .Name("strategy_account_snapshot_last_recv_ns")
             .Help("Unix ns timestamp of the last AccountSnapshot received per exchange")
             .Register(*registry);

    auto& mdt = prometheus::BuildCounter()
                    .Name("strategy_md_ticks_total")
                    .Help("Total MD ticks received from MdGateway")
                    .Register(*registry);
    md_ticks_total = &mdt.Add({});

    auto& er = prometheus::BuildCounter()
                   .Name("strategy_exec_reports_total")
                   .Help("Total ExecReports received from OrderGateway")
                   .Register(*registry);
    exec_reports_total = &er.Add({});

    refdata_ready_fam = &prometheus::BuildGauge()
                             .Name("strategy_refdata_ready")
                             .Help("1 if refdata has loaded for this exchange")
                             .Register(*registry);

    tick_to_strategy_ns_fam = &prometheus::BuildHistogram()
                                   .Name("strategy_tick_to_strategy_ns")
                                   .Help("Latency from MD tick timestamp to strategy callback return (ns)")
                                   .Register(*registry);

    tick_to_order_ns_fam = &prometheus::BuildHistogram()
                                .Name("strategy_tick_to_order_ns")
                                .Help("Latency from MD tick timestamp to order placed (ns)")
                                .Register(*registry);

    // Pre-allocate the unlabeled histograms and cache direct pointers so
    // hot-path Observe() calls avoid the Family::Add() hash lookup on
    // every tick.
    tick_to_strategy_ns_hist = &tick_to_strategy_ns();
    tick_to_order_ns_hist = &tick_to_order_ns();
}

}  // namespace bpt::strategy::metrics
