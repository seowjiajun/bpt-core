#pragma once

#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <unordered_map>

namespace bpt::strategy::metrics {

struct StrategyMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Gauge* healthy{};
    prometheus::Gauge* strategy_active{};
    prometheus::Gauge* trading_paused{};
    prometheus::Gauge* trading_halted{};
    prometheus::Gauge* refdata_stale{};
    prometheus::Counter* md_ticks_total{};
    prometheus::Counter* exec_reports_total{};
    prometheus::Counter* reconciliation_divergences_total{};

    // Pre-cached for hot-path Observe() — no Family lookup per tick.
    prometheus::Histogram* tick_to_strategy_ns_hist{};
    prometheus::Histogram* tick_to_order_ns_hist{};

    explicit StrategyMetrics(int port);

    void shutdown() {
        if (healthy)
            healthy->Set(0.0);
        if (strategy_active)
            strategy_active->Set(0.0);
    }

    prometheus::Gauge& refdata_ready(const std::string& exchange);
    prometheus::Gauge& account_snapshot_last_recv_ns(const std::string& exchange);

private:
    prometheus::Family<prometheus::Gauge>* refdata_ready_fam{};
    prometheus::Family<prometheus::Gauge>* account_snapshot_last_recv_ns_fam{};
    prometheus::Family<prometheus::Histogram>* tick_to_strategy_ns_fam{};
    prometheus::Family<prometheus::Histogram>* tick_to_order_ns_fam{};

    struct PerExchangeGauges {
        prometheus::Gauge* refdata_ready{};
        prometheus::Gauge* account_snapshot_last_recv_ns{};
    };
    std::unordered_map<std::string, PerExchangeGauges> exchange_cache_;
    PerExchangeGauges& exchange_entry(const std::string& exchange);

    static const prometheus::Histogram::BucketBoundaries& kDefaultLatencyBucketsNs();
};

}  // namespace bpt::strategy::metrics
