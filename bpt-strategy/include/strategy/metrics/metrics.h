#pragma once

#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>

namespace bpt::strategy::metrics {

// Prometheus metrics exposed by Strategy on the metrics_port from config.
// Mirrors the shape of HeimdallMetrics / MD GatewayMetrics for consistency
// across services — same struct-of-pointers layout, same shutdown()
// semantics, per-label families resolved via accessor methods.
//
// The struct takes a raw pointer stance (no unique_ptr to Gauge etc.)
// because prometheus-cpp owns the metric objects via the Registry;
// we just cache direct pointers for the hot-path writes.
struct StrategyMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    // Liveness / state gauges
    prometheus::Gauge* healthy{};          // 1 = process alive
    prometheus::Gauge* strategy_active{};  // 1 = strategy started (post startup gate)
    prometheus::Gauge* trading_paused{};   // 1 = trading paused (heartbeat stale)

    // Throughput counters
    prometheus::Counter* md_ticks_total{};      // every MD tick from bpt-md-gateway
    prometheus::Counter* exec_reports_total{};  // every ExecReport from order-gateway

    // Per-exchange families
    prometheus::Family<prometheus::Gauge>* refdata_ready_fam{};

    // Latency histograms — TSC-sourced, both in nanoseconds:
    //   tick_to_strategy_ns : MD tick timestamp → strategy callback returns
    //   tick_to_order_ns    : MD tick timestamp → order placed (subset —
    //                         only the ticks that actually produced an order)
    //
    // Pointers are cached at construction time (unlabeled {} family member)
    // so the hot-path call sites do a single deref + Observe(), not a hash
    // lookup through prometheus-cpp's Family map on every tick.
    prometheus::Family<prometheus::Histogram>* tick_to_strategy_ns_fam{};
    prometheus::Family<prometheus::Histogram>* tick_to_order_ns_fam{};
    prometheus::Histogram* tick_to_strategy_ns_hist{};
    prometheus::Histogram* tick_to_order_ns_hist{};

    explicit StrategyMetrics(int port);

    void shutdown() {
        if (healthy) healthy->Set(0.0);
        if (strategy_active) strategy_active->Set(0.0);
    }

    // Accessors — these allocate on first call via prometheus-cpp's Add().
    // Cache the returned reference at call sites if used in hot loops.
    prometheus::Gauge& refdata_ready(const std::string& exchange) {
        return refdata_ready_fam->Add({{"exchange", exchange}});
    }
    // Prefer the cached pointers (tick_to_*_ns_hist) for hot-path observes.
    // These accessors are here for completeness / ad-hoc construction only.
    prometheus::Histogram& tick_to_strategy_ns() {
        return tick_to_strategy_ns_fam->Add({}, kDefaultLatencyBucketsNs());
    }
    prometheus::Histogram& tick_to_order_ns() {
        return tick_to_order_ns_fam->Add({}, kDefaultLatencyBucketsNs());
    }

private:
    static const prometheus::Histogram::BucketBoundaries& kDefaultLatencyBucketsNs();
};

}  // namespace bpt::strategy::metrics
