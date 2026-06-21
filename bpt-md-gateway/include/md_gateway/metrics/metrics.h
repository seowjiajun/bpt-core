#pragma once

#include <bpt_common/util/latency_histogram.h>
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <string>
#include <unordered_map>

namespace bpt::md_gateway::metrics {

struct MdGatewayMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Gauge* healthy{};
    prometheus::Counter* subscription_batches_total{};
    prometheus::Counter* service_heartbeats_total{};
    prometheus::Counter* md_messages_dropped{};

    MdGatewayMetrics(const std::string& host, uint16_t port);

    void shutdown() {
        if (healthy)
            healthy->Set(0.0);
    }

    prometheus::Gauge& exchange_connected(const std::string& exchange);
    prometheus::Counter& adapter_disconnects(const std::string& exchange);
    prometheus::Gauge& md_messages_published(const std::string& exchange);
    prometheus::Gauge& md_validation_drops(const std::string& exchange);
    prometheus::Gauge& validation_drop_breaker_tripped(const std::string& exchange);

    void update_decode_latency(const std::string& exchange, bpt::common::util::LatencyHistogram& hist);

private:
    prometheus::Family<prometheus::Gauge>* exchange_connected_fam{};
    prometheus::Family<prometheus::Counter>* adapter_disconnects_fam{};
    prometheus::Family<prometheus::Gauge>* md_messages_published_fam{};
    prometheus::Family<prometheus::Gauge>* md_validation_drops_fam{};
    prometheus::Family<prometheus::Gauge>* validation_drop_breaker_tripped_fam{};

    prometheus::Family<prometheus::Gauge>* decode_latency_p50_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p95_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p99_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p999_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_max_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_mean_fam{};

    struct PerExchangeGauges {
        prometheus::Gauge* connected{};
        prometheus::Counter* disconnects{};
        prometheus::Gauge* published{};
        prometheus::Gauge* validation_drops{};
        prometheus::Gauge* breaker_tripped{};
    };
    struct DecodeLatencyGauges {
        prometheus::Gauge* p50{};
        prometheus::Gauge* p95{};
        prometheus::Gauge* p99{};
        prometheus::Gauge* p999{};
        prometheus::Gauge* max{};
        prometheus::Gauge* mean{};
    };

    std::unordered_map<std::string, PerExchangeGauges> exchange_cache_;
    std::unordered_map<std::string, DecodeLatencyGauges> decode_latency_cache_;

    PerExchangeGauges& exchange_entry(const std::string& exchange);
    DecodeLatencyGauges& decode_latency_entry(const std::string& exchange);
};

}  // namespace bpt::md_gateway::metrics
