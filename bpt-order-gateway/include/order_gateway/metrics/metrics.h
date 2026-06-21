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

namespace bpt::order_gateway::metrics {

struct OrderGatewayMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Gauge* healthy{};
    prometheus::Gauge* open_orders{};
    prometheus::Counter* stale_orders_total{};
    prometheus::Gauge* daily_loss_latched{};
    prometheus::Gauge* reject_rate_breaker_tripped{};

    explicit OrderGatewayMetrics(uint16_t port);

    void shutdown() {
        if (healthy)
            healthy->Set(0.0);
    }

    prometheus::Gauge& exchange_connected(const std::string& exchange);
    prometheus::Gauge& disconnect_breaker_tripped(const std::string& exchange);
    prometheus::Counter& orders_received(const std::string& exchange);
    prometheus::Counter& risk_reject(const std::string& exchange);
    prometheus::Counter& exec_report(const std::string& exchange, const std::string& status);
    prometheus::Histogram& order_ack_rtt(const std::string& exchange);

private:
    prometheus::Family<prometheus::Gauge>* disconnect_breaker_tripped_fam{};
    prometheus::Family<prometheus::Gauge>* exchange_connected_fam{};
    prometheus::Family<prometheus::Counter>* orders_received_fam{};
    prometheus::Family<prometheus::Counter>* risk_rejects_fam{};
    prometheus::Family<prometheus::Counter>* exec_reports_fam{};
    prometheus::Family<prometheus::Histogram>* order_ack_rtt_fam{};

    struct PerExchangeMetrics {
        prometheus::Gauge* connected{};
        prometheus::Gauge* disconnect_breaker{};
        prometheus::Counter* orders_received{};
        prometheus::Counter* risk_reject{};
        prometheus::Histogram* order_ack_rtt{};
        std::unordered_map<std::string, prometheus::Counter*> exec_reports_by_status;
    };
    std::unordered_map<std::string, PerExchangeMetrics> exchange_cache_;
    PerExchangeMetrics& exchange_entry(const std::string& exchange);

    static const prometheus::Histogram::BucketBoundaries& kAckRttBuckets();
};

}  // namespace bpt::order_gateway::metrics
