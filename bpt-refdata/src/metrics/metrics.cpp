#include "refdata/metrics/metrics.h"

#include <prometheus/counter.h>
#include <prometheus/gauge.h>

namespace bpt::refdata::metrics {

RefdataMetrics::RefdataMetrics(uint16_t port) {
    registry = std::make_shared<prometheus::Registry>();

    // port==0 disables the HTTP exposer — used by tests so a fixture
    // can spin up RefdataService without binding a TCP socket.
    if (port != 0) {
        exposer = std::make_unique<prometheus::Exposer>("0.0.0.0:" + std::to_string(port));
        exposer->RegisterCollectable(registry);
    }

    healthy = &prometheus::BuildGauge()
                   .Name("refdata_healthy")
                   .Help("1 if Refdata is running normally")
                   .Register(*registry)
                   .Add({});
    healthy->Set(1.0);

    instruments_total = &prometheus::BuildGauge()
                             .Name("refdata_instruments_total")
                             .Help("Total instruments known to the registry")
                             .Register(*registry)
                             .Add({});

    requests_served_total = &prometheus::BuildCounter()
                                 .Name("refdata_requests_served_total")
                                 .Help("Total RefDataSubscriptionRequest messages served")
                                 .Register(*registry)
                                 .Add({});

    last_update_ns = &prometheus::BuildGauge()
                          .Name("refdata_last_update_ns")
                          .Help("Unix ns timestamp of the most recent instrument publish (snapshot or delta)")
                          .Register(*registry)
                          .Add({});

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

RefdataMetrics::PerExchangeMetrics& RefdataMetrics::exchange_entry(const std::string& exchange) {
    auto it = exchange_cache_.find(exchange);
    if (it != exchange_cache_.end())
        return it->second;
    auto& e = exchange_cache_[exchange];
    e.exchange_ready = &exchange_ready_fam->Add({{"exchange", exchange}});
    e.snapshot_failures = &snapshot_failures_fam->Add({{"exchange", exchange}});
    e.listing_refreshes = &listing_refreshes_fam->Add({{"exchange", exchange}});
    e.fee_updates = &fee_updates_fam->Add({{"exchange", exchange}});
    return e;
}

prometheus::Gauge& RefdataMetrics::exchange_ready(const std::string& exchange) {
    return *exchange_entry(exchange).exchange_ready;
}
prometheus::Counter& RefdataMetrics::snapshot_failure(const std::string& exchange) {
    return *exchange_entry(exchange).snapshot_failures;
}
prometheus::Counter& RefdataMetrics::listing_refresh(const std::string& exchange) {
    return *exchange_entry(exchange).listing_refreshes;
}
prometheus::Counter& RefdataMetrics::fee_update(const std::string& exchange) {
    return *exchange_entry(exchange).fee_updates;
}

}  // namespace bpt::refdata::metrics
