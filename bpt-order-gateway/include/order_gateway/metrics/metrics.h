#pragma once

#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>

namespace bpt::order_gateway::metrics {

struct HeimdallMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Gauge* healthy{};
    prometheus::Gauge* open_orders{};
    prometheus::Counter* stale_orders_total{};

    // Per-exchange families
    prometheus::Family<prometheus::Gauge>* exchange_connected_fam{};
    prometheus::Family<prometheus::Counter>* orders_received_fam{};
    prometheus::Family<prometheus::Counter>* risk_rejects_fam{};
    prometheus::Family<prometheus::Counter>* exec_reports_fam{};
    // Order ACK round-trip time: order placed → first exec report received (ns)
    prometheus::Family<prometheus::Histogram>* order_ack_rtt_fam{};

    explicit HeimdallMetrics(uint16_t port);

    void shutdown() {
        if (healthy)
            healthy->Set(0.0);
    }

    prometheus::Gauge& exchange_connected(const std::string& exchange) {
        return exchange_connected_fam->Add({{"exchange", exchange}});
    }
    prometheus::Counter& orders_received(const std::string& exchange) {
        return orders_received_fam->Add({{"exchange", exchange}});
    }
    prometheus::Counter& risk_reject(const std::string& exchange) {
        return risk_rejects_fam->Add({{"exchange", exchange}});
    }
    prometheus::Counter& exec_report(const std::string& exchange, const std::string& status) {
        return exec_reports_fam->Add({{"exchange", exchange}, {"status", status}});
    }
    prometheus::Histogram& order_ack_rtt(const std::string& exchange) {
        // Buckets: 1ms, 5ms, 10ms, 25ms, 50ms, 100ms, 250ms, 500ms, 1s, 2.5s, 5s
        return order_ack_rtt_fam->Add(
            {{"exchange", exchange}},
            prometheus::Histogram::BucketBoundaries{1e6, 5e6, 10e6, 25e6, 50e6, 100e6, 250e6, 500e6, 1e9, 2.5e9, 5e9});
    }
};

}  // namespace bpt::order_gateway::metrics
