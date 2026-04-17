#include "order_gateway/metrics/metrics.h"

#include <prometheus/histogram.h>

namespace bpt::order_gateway::metrics {

HeimdallMetrics::HeimdallMetrics(uint16_t port) {
    registry = std::make_shared<prometheus::Registry>();

    exposer = std::make_unique<prometheus::Exposer>("0.0.0.0:" + std::to_string(port));
    exposer->RegisterCollectable(registry);

    auto& h = prometheus::BuildGauge().Name("order_gateway_healthy").Help("1 if Heimdall is running").Register(*registry);
    healthy = &h.Add({});
    healthy->Set(1.0);

    auto& oo = prometheus::BuildGauge()
                   .Name("order_gateway_open_orders")
                   .Help("Current number of open orders across all venues")
                   .Register(*registry);
    open_orders = &oo.Add({});

    auto& st = prometheus::BuildCounter()
                   .Name("order_gateway_stale_orders_total")
                   .Help("Orders cancelled due to stale timeout")
                   .Register(*registry);
    stale_orders_total = &st.Add({});

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

}  // namespace bpt::order_gateway::metrics
