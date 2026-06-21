#pragma once

#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <string>
#include <unordered_map>

namespace bpt::refdata::metrics {

struct RefdataMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Gauge* healthy{};
    prometheus::Gauge* instruments_total{};
    prometheus::Counter* requests_served_total{};
    prometheus::Gauge* last_update_ns{};

    explicit RefdataMetrics(uint16_t port);

    void shutdown() {
        if (healthy)
            healthy->Set(0.0);
    }

    prometheus::Gauge& exchange_ready(const std::string& exchange);
    prometheus::Counter& snapshot_failure(const std::string& exchange);
    prometheus::Counter& listing_refresh(const std::string& exchange);
    prometheus::Counter& fee_update(const std::string& exchange);

private:
    prometheus::Family<prometheus::Gauge>* exchange_ready_fam{};
    prometheus::Family<prometheus::Counter>* snapshot_failures_fam{};
    prometheus::Family<prometheus::Counter>* listing_refreshes_fam{};
    prometheus::Family<prometheus::Counter>* fee_updates_fam{};

    struct PerExchangeMetrics {
        prometheus::Gauge* exchange_ready{};
        prometheus::Counter* snapshot_failures{};
        prometheus::Counter* listing_refreshes{};
        prometheus::Counter* fee_updates{};
    };
    std::unordered_map<std::string, PerExchangeMetrics> exchange_cache_;
    PerExchangeMetrics& exchange_entry(const std::string& exchange);
};

}  // namespace bpt::refdata::metrics
