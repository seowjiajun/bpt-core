#include "md_gateway/metrics/metrics.h"

namespace bpt::md_gateway::metrics {

MdGatewayMetrics::MdGatewayMetrics(const std::string& host, uint16_t port) {
    registry = std::make_shared<prometheus::Registry>();
    exposer = std::make_unique<prometheus::Exposer>(host + ":" + std::to_string(port));
    exposer->RegisterCollectable(registry);

    healthy = &prometheus::BuildGauge()
                   .Name("bpt_md_gateway_healthy")
                   .Help("1 if MdGateway is running")
                   .Register(*registry)
                   .Add({});
    healthy->Set(1.0);

    subscription_batches_total = &prometheus::BuildCounter()
                                      .Name("bpt_md_gateway_subscription_batches_total")
                                      .Help("Total MdSubscribeBatch messages received from Strategy")
                                      .Register(*registry)
                                      .Add({});

    service_heartbeats_total = &prometheus::BuildCounter()
                                    .Name("bpt_md_gateway_service_heartbeats_total")
                                    .Help("Total MdServiceHeartbeat messages published")
                                    .Register(*registry)
                                    .Add({});

    md_messages_dropped = &prometheus::BuildCounter()
                               .Name("bpt_md_gateway_md_messages_dropped_total")
                               .Help("Cumulative MD messages dropped due to Aeron backpressure")
                               .Register(*registry)
                               .Add({});

    exchange_connected_fam = &prometheus::BuildGauge()
                                  .Name("bpt_md_gateway_exchange_connected")
                                  .Help("1 if the adapter is connected to this exchange")
                                  .Register(*registry);

    adapter_disconnects_fam = &prometheus::BuildCounter()
                                   .Name("bpt_md_gateway_adapter_disconnects_total")
                                   .Help("Total unexpected disconnects per exchange adapter")
                                   .Register(*registry);

    md_messages_published_fam = &prometheus::BuildGauge()
                                     .Name("bpt_md_gateway_md_messages_published_total")
                                     .Help("Total MD messages published per exchange (monotonic)")
                                     .Register(*registry);

    md_validation_drops_fam = &prometheus::BuildGauge()
                                   .Name("bpt_md_gateway_md_validation_drops_total")
                                   .Help("Total MD messages dropped by MdValidator per exchange")
                                   .Register(*registry);

    validation_drop_breaker_tripped_fam =
        &prometheus::BuildGauge()
             .Name("bpt_md_gateway_validation_drop_breaker_tripped")
             .Help("1 if the per-adapter validation-drop breaker has tripped (forwarding halted)")
             .Register(*registry);

    decode_latency_p50_fam = &prometheus::BuildGauge()
                                  .Name("bpt_md_gateway_bbo_decode_p50_ns")
                                  .Help("BBO decode latency p50 (ns) per exchange")
                                  .Register(*registry);
    decode_latency_p95_fam = &prometheus::BuildGauge()
                                  .Name("bpt_md_gateway_bbo_decode_p95_ns")
                                  .Help("BBO decode latency p95 (ns) per exchange")
                                  .Register(*registry);
    decode_latency_p99_fam = &prometheus::BuildGauge()
                                  .Name("bpt_md_gateway_bbo_decode_p99_ns")
                                  .Help("BBO decode latency p99 (ns) per exchange")
                                  .Register(*registry);
    decode_latency_p999_fam = &prometheus::BuildGauge()
                                   .Name("bpt_md_gateway_bbo_decode_p999_ns")
                                   .Help("BBO decode latency p99.9 (ns) per exchange")
                                   .Register(*registry);
    decode_latency_max_fam = &prometheus::BuildGauge()
                                  .Name("bpt_md_gateway_bbo_decode_max_ns")
                                  .Help("BBO decode latency max (ns) per exchange")
                                  .Register(*registry);
    decode_latency_mean_fam = &prometheus::BuildGauge()
                                   .Name("bpt_md_gateway_bbo_decode_mean_ns")
                                   .Help("BBO decode latency mean (ns) per exchange")
                                   .Register(*registry);
}

MdGatewayMetrics::PerExchangeGauges& MdGatewayMetrics::exchange_entry(const std::string& exchange) {
    auto it = exchange_cache_.find(exchange);
    if (it != exchange_cache_.end())
        return it->second;
    auto& e = exchange_cache_[exchange];
    e.connected = &exchange_connected_fam->Add({{"exchange", exchange}});
    e.disconnects = &adapter_disconnects_fam->Add({{"exchange", exchange}});
    e.published = &md_messages_published_fam->Add({{"exchange", exchange}});
    e.validation_drops = &md_validation_drops_fam->Add({{"exchange", exchange}});
    e.breaker_tripped = &validation_drop_breaker_tripped_fam->Add({{"exchange", exchange}});
    return e;
}

MdGatewayMetrics::DecodeLatencyGauges& MdGatewayMetrics::decode_latency_entry(const std::string& exchange) {
    auto it = decode_latency_cache_.find(exchange);
    if (it != decode_latency_cache_.end())
        return it->second;
    auto& e = decode_latency_cache_[exchange];
    e.p50 = &decode_latency_p50_fam->Add({{"exchange", exchange}});
    e.p95 = &decode_latency_p95_fam->Add({{"exchange", exchange}});
    e.p99 = &decode_latency_p99_fam->Add({{"exchange", exchange}});
    e.p999 = &decode_latency_p999_fam->Add({{"exchange", exchange}});
    e.max = &decode_latency_max_fam->Add({{"exchange", exchange}});
    e.mean = &decode_latency_mean_fam->Add({{"exchange", exchange}});
    return e;
}

prometheus::Gauge& MdGatewayMetrics::exchange_connected(const std::string& exchange) {
    return *exchange_entry(exchange).connected;
}
prometheus::Counter& MdGatewayMetrics::adapter_disconnects(const std::string& exchange) {
    return *exchange_entry(exchange).disconnects;
}
prometheus::Gauge& MdGatewayMetrics::md_messages_published(const std::string& exchange) {
    return *exchange_entry(exchange).published;
}
prometheus::Gauge& MdGatewayMetrics::md_validation_drops(const std::string& exchange) {
    return *exchange_entry(exchange).validation_drops;
}
prometheus::Gauge& MdGatewayMetrics::validation_drop_breaker_tripped(const std::string& exchange) {
    return *exchange_entry(exchange).breaker_tripped;
}

void MdGatewayMetrics::update_decode_latency(const std::string& exchange, bpt::common::util::LatencyHistogram& hist) {
    auto snap = hist.snapshot_and_reset();
    if (snap.total == 0)
        return;
    auto& e = decode_latency_entry(exchange);
    e.p50->Set(static_cast<double>(snap.percentile_ns(0.50)));
    e.p95->Set(static_cast<double>(snap.percentile_ns(0.95)));
    e.p99->Set(static_cast<double>(snap.percentile_ns(0.99)));
    e.p999->Set(static_cast<double>(snap.percentile_ns(0.999)));
    e.max->Set(static_cast<double>(snap.max_ns()));
    e.mean->Set(static_cast<double>(snap.mean_ns()));
}

}  // namespace bpt::md_gateway::metrics
