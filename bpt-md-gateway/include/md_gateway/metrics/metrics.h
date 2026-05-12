#pragma once

/// \file
/// \brief Prometheus metric registry + per-exchange families for `bpt-md-gateway`.
///
/// One MdGatewayMetrics instance per process. Owns the prometheus-cpp
/// `Registry` and `Exposer` (HTTP server on `metrics_host:metrics_port`)
/// and pre-registers every Family the app updates. Per-instrument
/// labels are deliberately avoided — instrument-level cardinality
/// would explode under any non-trivial universe — so the granular
/// dimension on every gauge/counter is `exchange`.

#include <bpt_common/util/latency_histogram.h>
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <string>

namespace bpt::md_gateway::metrics {

/// \brief Registry + helper accessors for every metric the gateway emits.
struct MdGatewayMetrics {
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Gauge* healthy{};  ///< 1 while the app is running, set to 0 by `shutdown()`
    prometheus::Counter* subscription_batches_total{};
    prometheus::Counter* service_heartbeats_total{};

    /// \brief Total MD messages dropped due to Aeron backpressure (all exchanges combined).
    prometheus::Gauge* md_messages_dropped{};

    /// \name Per-exchange families
    /// @{
    prometheus::Family<prometheus::Gauge>* exchange_connected_fam{};     ///< 1 if WS connected, 0 otherwise
    prometheus::Family<prometheus::Counter>* adapter_disconnects_fam{};  ///< monotonic disconnect count
    prometheus::Family<prometheus::Gauge>*
        md_messages_published_fam{};  ///< messages that passed validation and were sent to Aeron
    prometheus::Family<prometheus::Gauge>*
        md_validation_drops_fam{};  ///< messages dropped by MdValidator (bad prices, crossed book, …)
    /// \brief Per-adapter ValidationDropBreaker latch.
    ///
    /// 1 if the drop ratio exceeded the configured threshold and the
    /// ValidatingPublisher has stopped forwarding to Aeron. Latches;
    /// operator restart required to clear.
    prometheus::Family<prometheus::Gauge>* validation_drop_breaker_tripped_fam{};
    /// @}

    /// \name Per-exchange decode latency percentile gauges (nanoseconds)
    /// \brief Updated periodically by snapshotting each parser's LatencyHistogram.
    /// @{
    prometheus::Family<prometheus::Gauge>* decode_latency_p50_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p95_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p99_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_p999_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_max_fam{};
    prometheus::Family<prometheus::Gauge>* decode_latency_mean_fam{};
    /// @}

    /// \brief Construct + start the HTTP exposer on `host:port`.
    MdGatewayMetrics(const std::string& host, uint16_t port);

    /// \brief Mark the service as unhealthy. Called from MdGatewayApp::stop().
    void shutdown() {
        if (healthy)
            healthy->Set(0.0);
    }

    /// \brief Get-or-add the per-exchange `connected` gauge.
    prometheus::Gauge& exchange_connected(const std::string& exchange) {
        return exchange_connected_fam->Add({{"exchange", exchange}});
    }
    /// \brief Get-or-add the per-exchange disconnect counter.
    prometheus::Counter& adapter_disconnects(const std::string& exchange) {
        return adapter_disconnects_fam->Add({{"exchange", exchange}});
    }
    /// \brief Get-or-add the per-exchange "messages published" gauge.
    prometheus::Gauge& md_messages_published(const std::string& exchange) {
        return md_messages_published_fam->Add({{"exchange", exchange}});
    }
    /// \brief Get-or-add the per-exchange "validation drops" gauge.
    prometheus::Gauge& md_validation_drops(const std::string& exchange) {
        return md_validation_drops_fam->Add({{"exchange", exchange}});
    }
    /// \brief Get-or-add the per-exchange breaker-tripped gauge.
    prometheus::Gauge& validation_drop_breaker_tripped(const std::string& exchange) {
        return validation_drop_breaker_tripped_fam->Add({{"exchange", exchange}});
    }

    /// \brief Snapshot a parser's LatencyHistogram into the per-exchange gauge family.
    ///
    /// Resets the histogram after snapshotting; called once per
    /// reporter tick so each datapoint represents fresh samples.
    void update_decode_latency(const std::string& exchange, bpt::common::util::LatencyHistogram& hist) {
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
