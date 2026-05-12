#include "order_gateway/metrics/metrics.h"

#include <prometheus/histogram.h>

namespace bpt::order_gateway::metrics {

OrderGatewayMetrics::OrderGatewayMetrics(uint16_t port) {
    registry = std::make_shared<prometheus::Registry>();

    // port == 0 skips Exposer creation — used by unit tests that just
    // need counter pointers populated. Production callers always pass
    // a real port (defaulted in config).
    if (port != 0) {
        exposer = std::make_unique<prometheus::Exposer>("0.0.0.0:" + std::to_string(port));
        exposer->RegisterCollectable(registry);
    }

    auto& h =
        prometheus::BuildGauge().Name("order_gateway_healthy").Help("1 if OrderGateway is running").Register(*registry);
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

    // Risk / breaker latch gauges. Initialise to 0; flipped to 1 at trip time.
    auto& dll = prometheus::BuildGauge()
                    .Name("order_gateway_daily_loss_latched")
                    .Help("1 if daily-loss kill switch has latched — trading disabled until restart")
                    .Register(*registry);
    daily_loss_latched = &dll.Add({});

    auto& rrb = prometheus::BuildGauge()
                    .Name("order_gateway_reject_rate_breaker_tripped")
                    .Help("1 if global reject-rate breaker has tripped")
                    .Register(*registry);
    reject_rate_breaker_tripped = &rrb.Add({});

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

}  // namespace bpt::order_gateway::metrics
