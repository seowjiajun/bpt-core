#include "refdata/metrics/metrics.h"

#include <prometheus/counter.h>
#include <prometheus/gauge.h>

namespace bpt::refdata::metrics {

RefdataMetrics::RefdataMetrics(uint16_t port) {
    registry = std::make_shared<prometheus::Registry>();

    // port==0 disables the HTTP exposer — used by tests so a fixture
    // can spin up RefdataApp without binding a TCP socket.
    if (port != 0) {
        exposer = std::make_unique<prometheus::Exposer>("0.0.0.0:" + std::to_string(port));
        exposer->RegisterCollectable(registry);
    }

    auto& healthy_fam =
        prometheus::BuildGauge().Name("refdata_healthy").Help("1 if Refdata is running normally").Register(*registry);
    healthy = &healthy_fam.Add({});
    healthy->Set(1.0);

    auto& inst_fam = prometheus::BuildGauge()
                         .Name("refdata_instruments_total")
                         .Help("Total instruments known to the registry")
                         .Register(*registry);
    instruments_total = &inst_fam.Add({});

    auto& req_fam = prometheus::BuildCounter()
                        .Name("refdata_requests_served_total")
                        .Help("Total RefDataSubscriptionRequest messages served")
                        .Register(*registry);
    requests_served_total = &req_fam.Add({});

    auto& lu = prometheus::BuildGauge()
                   .Name("refdata_last_update_ns")
                   .Help("Unix ns timestamp of the most recent instrument publish (snapshot or delta)")
                   .Register(*registry);
    last_update_ns = &lu.Add({});

    exchange_ready_fam = &prometheus::BuildGauge()
                              .Name("refdata_exchange_ready")
                              .Help("1 if initial snapshot completed for this exchange")
                              .Register(*registry);

    snapshot_failures_fam = &prometheus::BuildCounter()
                                 .Name("refdata_snapshot_failures_total")
                                 .Help("Total snapshot failures per exchange")
                                 .Register(*registry);

    listing_refreshes_fam = &prometheus::BuildCounter()
                                 .Name("refdata_listing_refreshes_total")
                                 .Help("Total instrument listing refreshes per exchange")
                                 .Register(*registry);

    fee_updates_fam = &prometheus::BuildCounter()
                           .Name("refdata_fee_schedule_updates_total")
                           .Help("Total fee schedule updates published per exchange")
                           .Register(*registry);
}

}  // namespace bpt::refdata::metrics
