#pragma once

#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <string>

namespace bpt::refdata::metrics {

struct RefdataMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    // Unlabelled metrics
    prometheus::Gauge* healthy{};
    prometheus::Gauge* instruments_total{};
    prometheus::Counter* requests_served_total{};

    // Per-exchange metric families (use Add({{"exchange","X"}}) to get child)
    prometheus::Family<prometheus::Gauge>* exchange_ready_fam{};
    prometheus::Family<prometheus::Counter>* snapshot_failures_fam{};
    prometheus::Family<prometheus::Counter>* listing_refreshes_fam{};
    prometheus::Family<prometheus::Counter>* fee_updates_fam{};

    explicit RefdataMetrics(uint16_t port);

    void shutdown() {
        if (healthy)
            healthy->Set(0.0);
    }

    prometheus::Gauge& exchange_ready(const std::string& exchange) {
        return exchange_ready_fam->Add({{"exchange", exchange}});
    }
    prometheus::Counter& snapshot_failure(const std::string& exchange) {
        return snapshot_failures_fam->Add({{"exchange", exchange}});
    }
    prometheus::Counter& listing_refresh(const std::string& exchange) {
        return listing_refreshes_fam->Add({{"exchange", exchange}});
    }
    prometheus::Counter& fee_update(const std::string& exchange) {
        return fee_updates_fam->Add({{"exchange", exchange}});
    }
};

}  // namespace bpt::refdata::metrics
