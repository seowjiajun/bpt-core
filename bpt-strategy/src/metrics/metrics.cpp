#include "strategy/metrics/metrics.h"

#include <prometheus/histogram.h>

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

    healthy =
        &prometheus::BuildGauge().Name("strategy_healthy").Help("1 if Strategy is running").Register(*registry).Add({});
    healthy->Set(1.0);

    strategy_active = &prometheus::BuildGauge()
                           .Name("strategy_strategy_active")
                           .Help("1 if the strategy has started (past the startup gate)")
                           .Register(*registry)
                           .Add({});
    strategy_active->Set(0.0);

    trading_paused = &prometheus::BuildGauge()
                          .Name("strategy_trading_paused")
                          .Help("1 if trading is paused (service heartbeat stale or kill switch)")
                          .Register(*registry)
                          .Add({});
    trading_paused->Set(0.0);

    trading_halted = &prometheus::BuildGauge()
                          .Name("strategy_trading_halted")
                          .Help("1 if console kill-switch latched — no automatic recovery")
                          .Register(*registry)
                          .Add({});
    trading_halted->Set(0.0);

    refdata_stale = &prometheus::BuildGauge()
                         .Name("strategy_refdata_stale")
                         .Help("1 if strategy detected stale refdata heartbeat and paused new quotes")
                         .Register(*registry)
                         .Add({});
    refdata_stale->Set(0.0);

    reconciliation_divergences_total =
        &prometheus::BuildCounter()
             .Name("strategy_reconciliation_divergences_total")
             .Help("Count of reconciliation passes that produced at least one divergence")
             .Register(*registry)
             .Add({});

    md_ticks_total = &prometheus::BuildCounter()
                          .Name("strategy_md_ticks_total")
                          .Help("Total MD ticks received from MdGateway")
                          .Register(*registry)
                          .Add({});

    exec_reports_total = &prometheus::BuildCounter()
                              .Name("strategy_exec_reports_total")
                              .Help("Total ExecReports received from OrderGateway")
                              .Register(*registry)
                              .Add({});

    refdata_ready_fam = &prometheus::BuildGauge()
                             .Name("strategy_refdata_ready")
                             .Help("1 if refdata has loaded for this exchange")
                             .Register(*registry);

    account_snapshot_last_recv_ns_fam =
        &prometheus::BuildGauge()
             .Name("strategy_account_snapshot_last_recv_ns")
             .Help("Unix ns timestamp of the last AccountSnapshot received per exchange")
             .Register(*registry);

    tick_to_strategy_ns_fam = &prometheus::BuildHistogram()
                                   .Name("strategy_tick_to_strategy_ns")
                                   .Help("Latency from MD tick timestamp to strategy callback return (ns)")
                                   .Register(*registry);

    tick_to_order_ns_fam = &prometheus::BuildHistogram()
                                .Name("strategy_tick_to_order_ns")
                                .Help("Latency from MD tick timestamp to order placed (ns)")
                                .Register(*registry);

    tick_to_strategy_ns_hist = &tick_to_strategy_ns_fam->Add({}, kDefaultLatencyBucketsNs());
    tick_to_order_ns_hist = &tick_to_order_ns_fam->Add({}, kDefaultLatencyBucketsNs());
}

StrategyMetrics::PerExchangeGauges& StrategyMetrics::exchange_entry(const std::string& exchange) {
    auto it = exchange_cache_.find(exchange);
    if (it != exchange_cache_.end())
        return it->second;
    auto& e = exchange_cache_[exchange];
    e.refdata_ready = &refdata_ready_fam->Add({{"exchange", exchange}});
    e.account_snapshot_last_recv_ns = &account_snapshot_last_recv_ns_fam->Add({{"exchange", exchange}});
    return e;
}

prometheus::Gauge& StrategyMetrics::refdata_ready(const std::string& exchange) {
    return *exchange_entry(exchange).refdata_ready;
}
prometheus::Gauge& StrategyMetrics::account_snapshot_last_recv_ns(const std::string& exchange) {
    return *exchange_entry(exchange).account_snapshot_last_recv_ns;
}

}  // namespace bpt::strategy::metrics
