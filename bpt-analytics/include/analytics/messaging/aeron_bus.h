#pragma once

/// @file
/// Bus boundary for bpt-analytics. Mirrors the shape used by
/// bpt-refdata, bpt-md-gateway, bpt-order-gateway, bpt-strategy, and
/// bpt-pricer: every concrete Aeron pub/sub the app needs is built in
/// one factory so `AnalyticsApp` doesn't take `<Aeron.h>` in its
/// constructor.

#include "analytics/messaging/exec_report_subscriber.h"
#include "analytics/messaging/md_bbo_subscriber.h"
#include "analytics/messaging/toxicity_publisher.h"

#include <Aeron.h>

#include <memory>

namespace bpt::analytics {
namespace config {
struct Settings;
}

namespace messaging {

struct AnalyticsBus {
    std::unique_ptr<ExecReportSubscriber> exec_sub;
    std::unique_ptr<MdBboSubscriber> md_sub;
    std::unique_ptr<ToxicityPublisher> tox_pub;
};

class AnalyticsAeronBus {
public:
    /// Build every Aeron-touching object analytics needs. Sole place
    /// that calls into `<Aeron.h>` from the application layer.
    static AnalyticsBus build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::analytics
