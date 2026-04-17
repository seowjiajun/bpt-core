#pragma once

#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <string>
#include <yggdrasil/util/latency_histogram.h>

namespace bpt::md_gateway::metrics {

struct MdGatewayMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Gauge* healthy{};
    prometheus::Counter* subscription_batches_total{};
    prometheus::Counter* service_heartbeats_total{};

    // Total MD messages dropped due to Aeron backpressure (all exchanges combined).
    prometheus::Gauge* md_messages_dropped{};

    // Per-exchange families
    prometheus::Family<prometheus::Gauge>* exchange_connected_fam{};
    prometheus::Family<prometheus::Counter>* adapter_disconnects_fam{};
    // Total MD messages that passed validation and were sent to Aeron.
    prometheus::Family<prometheus::Gauge>* md_messages_published_fam{};
    // Total MD messages dropped by MdValidator (bad prices, crossed book, etc.).
    prometheus::Family<prometheus::Gauge>* md_validation_drops_fam{};

    // Per-exchange decode latency percentile gauges (nanoseconds).
    // Updated periodically by snapshotting each parser's LatencyHistogram.
    prometheus::Family<prometheus::Gauge>* decode_latency_p50_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p95_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p99_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p999_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_max_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_mean_fam{};

    MdGatewayMetrics(const std::string& host, uint16_t port);

    void shutdown() {
        if (healthy)
            healthy->Set(0.0);
    }

    prometheus::Gauge& exchange_connected(const std::string& exchange) {
        return exchange_connected_fam->Add({{"exchange", exchange}});
    }
    prometheus::Counter& adapter_disconnects(const std::string& exchange) {
        return adapter_disconnects_fam->Add({{"exchange", exchange}});
    }
    prometheus::Gauge& md_messages_published(const std::string& exchange) {
        return md_messages_published_fam->Add({{"exchange", exchange}});
    }
    prometheus::Gauge& md_validation_drops(const std::string& exchange) {
        return md_validation_drops_fam->Add({{"exchange", exchange}});
    }

    // Record a histogram snapshot into the latency gauge family for one exchange.
    void update_decode_latency(const std::string& exchange, ygg::util::LatencyHistogram& hist) {
        auto snap = hist.snapshot_and_reset();
        if (snap.total == 0)
            return;
        decode_latency_p50_fam->Add({{"exchange", exchange}}).Set(static_cast<double>(snap.percentile_ns(0.50)));
        decode_latency_p95_fam->Add({{"exchange", exchange}}).Set(static_cast<double>(snap.percentile_ns(0.95)));
        decode_latency_p99_fam->Add({{"exchange", exchange}}).Set(static_cast<double>(snap.percentile_ns(0.99)));
        decode_latency_p999_fam->Add({{"exchange", exchange}}).Set(static_cast<double>(snap.percentile_ns(0.999)));
        decode_latency_max_fam->Add({{"exchange", exchange}}).Set(static_cast<double>(snap.max_ns()));
        decode_latency_mean_fam->Add({{"exchange", exchange}}).Set(static_cast<double>(snap.mean_ns()));
    }
};

}  // namespace bpt::md_gateway::metrics
