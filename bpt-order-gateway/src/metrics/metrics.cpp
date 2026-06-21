#include "order_gateway/metrics/metrics.h"

#include <prometheus/histogram.h>

namespace bpt::order_gateway::metrics {

const prometheus::Histogram::BucketBoundaries& OrderGatewayMetrics::kAckRttBuckets() {
    // 1ms → 5s; covers normal exchange RTT and pathological tail latency.
    static const prometheus::Histogram::BucketBoundaries k{
        1e6,
        5e6,
        10e6,
        25e6,
        50e6,
        100e6,
        250e6,
        500e6,
        1e9,
        2.5e9,
        5e9,
    };
    return k;
}

OrderGatewayMetrics::OrderGatewayMetrics(uint16_t port) {
    registry = std::make_shared<prometheus::Registry>();

    // port == 0 skips Exposer creation — used by unit tests that just
    // need counter pointers populated without binding a TCP socket.
    if (port != 0) {
        exposer = std::make_unique<prometheus::Exposer>("0.0.0.0:" + std::to_string(port));
        exposer->RegisterCollectable(registry);
    }

    healthy = &prometheus::BuildGauge()
                   .Name("order_gateway_healthy")
                   .Help("1 if OrderGateway is running")
                   .Register(*registry)
                   .Add({});
    healthy->Set(1.0);

    open_orders = &prometheus::BuildGauge()
                       .Name("order_gateway_open_orders")
                       .Help("Current number of open orders across all venues")
                       .Register(*registry)
                       .Add({});

    stale_orders_total = &prometheus::BuildCounter()
                              .Name("order_gateway_stale_orders_total")
                              .Help("Orders cancelled due to stale timeout")
                              .Register(*registry)
                              .Add({});

    daily_loss_latched = &prometheus::BuildGauge()
                              .Name("order_gateway_daily_loss_latched")
                              .Help("1 if daily-loss kill switch has latched — trading disabled until restart")
                              .Register(*registry)
                              .Add({});

    reject_rate_breaker_tripped = &prometheus::BuildGauge()
                                       .Name("order_gateway_reject_rate_breaker_tripped")
                                       .Help("1 if global reject-rate breaker has tripped")
                                       .Register(*registry)
                                       .Add({});

    disconnect_breaker_tripped_fam = &prometheus::BuildGauge()
                                          .Name("order_gateway_disconnect_breaker_tripped")
                                          .Help("1 if the per-adapter disconnect-rate breaker has tripped")
                                          .Register(*registry);

    exchange_connected_fam = &prometheus::BuildGauge()
                                  .Name("order_gateway_exchange_connected")
                                  .Help("1 if adapter is connected to this exchange")
                                  .Register(*registry);

    orders_received_fam = &prometheus::BuildCounter()
                               .Name("order_gateway_orders_received_total")
                               .Help("Total NewOrder messages received from Strategy per exchange")
                               .Register(*registry);

    risk_rejects_fam = &prometheus::BuildCounter()
                            .Name("order_gateway_risk_rejects_total")
                            .Help("Total orders rejected by risk checks per exchange")
                            .Register(*registry);

    exec_reports_fam = &prometheus::BuildCounter()
                            .Name("order_gateway_exec_reports_total")
                            .Help("Total execution reports received per exchange and status")
                            .Register(*registry);

    order_ack_rtt_fam = &prometheus::BuildHistogram()
                             .Name("order_gateway_order_ack_rtt_ns")
                             .Help("Order acknowledgement RTT: time from NewOrder sent to first exec report (ns)")
                             .Register(*registry);
}

OrderGatewayMetrics::PerExchangeMetrics& OrderGatewayMetrics::exchange_entry(const std::string& exchange) {
    auto it = exchange_cache_.find(exchange);
    if (it != exchange_cache_.end())
        return it->second;
    auto& e = exchange_cache_[exchange];
    e.connected = &exchange_connected_fam->Add({{"exchange", exchange}});
    e.disconnect_breaker = &disconnect_breaker_tripped_fam->Add({{"exchange", exchange}});
    e.orders_received = &orders_received_fam->Add({{"exchange", exchange}});
    e.risk_reject = &risk_rejects_fam->Add({{"exchange", exchange}});
    e.order_ack_rtt = &order_ack_rtt_fam->Add({{"exchange", exchange}}, kAckRttBuckets());
    return e;
}

prometheus::Gauge& OrderGatewayMetrics::exchange_connected(const std::string& exchange) {
    return *exchange_entry(exchange).connected;
}
prometheus::Gauge& OrderGatewayMetrics::disconnect_breaker_tripped(const std::string& exchange) {
    return *exchange_entry(exchange).disconnect_breaker;
}
prometheus::Counter& OrderGatewayMetrics::orders_received(const std::string& exchange) {
    return *exchange_entry(exchange).orders_received;
}
prometheus::Counter& OrderGatewayMetrics::risk_reject(const std::string& exchange) {
    return *exchange_entry(exchange).risk_reject;
}
prometheus::Histogram& OrderGatewayMetrics::order_ack_rtt(const std::string& exchange) {
    return *exchange_entry(exchange).order_ack_rtt;
}

prometheus::Counter& OrderGatewayMetrics::exec_report(const std::string& exchange, const std::string& status) {
    auto& by_status = exchange_entry(exchange).exec_reports_by_status;
    auto it = by_status.find(status);
    if (it != by_status.end())
        return *it->second;
    auto* c = &exec_reports_fam->Add({{"exchange", exchange}, {"status", status}});
    by_status[status] = c;
    return *c;
}

}  // namespace bpt::order_gateway::metrics
