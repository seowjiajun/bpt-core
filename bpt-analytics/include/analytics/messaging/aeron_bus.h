#pragma once

/// @file
/// Bus boundary for bpt-analytics. Mirrors the shape used by
/// bpt-refdata, bpt-md-gateway, bpt-order-gateway, bpt-strategy, and
/// bpt-pricer: every concrete Aeron pub/sub the app needs is built in
/// one factory so `AnalyticsService` doesn't take `<Aeron.h>` in its
/// constructor.

#include "analytics/messaging/publishers/api/toxicity_publisher.h"
#include "analytics/messaging/subscribers/aeron/exec_report_subscriber.h"
#include "analytics/messaging/subscribers/aeron/md_bbo_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::analytics {
class AnalyticsService;
namespace config {
struct Settings;
}

namespace messaging {

/// Each subscriber is the concrete CRTP-templated instantiation on
/// AnalyticsService. Templated dispatch directly into
/// AnalyticsService::on_bbo / on_exec_report — zero std::function
/// indirection per tick.
struct AnalyticsBus {
    std::unique_ptr<aeron::ExecReportSubscriber<AnalyticsService>> exec_sub;
    std::unique_ptr<aeron::MdBboSubscriber<AnalyticsService>> md_sub;
    std::unique_ptr<api::ToxicityPublisher> tox_pub;
};

class AnalyticsAeronBus {
public:
    /// Build every Aeron-touching object analytics needs. Sole place
    /// that calls into `<Aeron.h>` from the application layer.
    static AnalyticsBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::analytics
