#pragma once

/// \file
/// \brief Composition root for the Aeron-backed bus adapters.
///
/// Bundles construction of all five Aeron publishers/subscribers so main.cpp wires
/// "the bus" as one unit rather than five inline `make_unique` calls. Adding
/// a new port = one field + one line in build(); main.cpp doesn't change.

#include "refdata/messaging/publishers/api/fee_schedule_publisher.h"
#include "refdata/messaging/subscribers/api/refdata_control_subscriber.h"
#include "refdata/messaging/publishers/api/refdata_delta_publisher.h"
#include "refdata/messaging/publishers/api/refdata_snapshot_publisher.h"
#include "refdata/messaging/publishers/api/refdata_status_publisher.h"

#include <Aeron.h>

#include <memory>

namespace bpt::refdata::config {
struct Settings;
}

namespace bpt::refdata::messaging {

struct RefdataBus {
    std::unique_ptr<messaging::api::RefdataControlSubscriber> control_sub;
    std::unique_ptr<messaging::api::RefdataSnapshotPublisher> snapshot_pub;
    std::shared_ptr<messaging::api::RefdataDeltaPublisher> delta_pub;
    std::shared_ptr<messaging::api::FeeSchedulePublisher> fee_pub;
    std::shared_ptr<messaging::api::RefdataStatusPublisher> status_pub;
};

class RefdataAeronBus {
public:
    /// \brief Build all five Aeron-backed adapters wired to the channels
    ///        and stream IDs in `settings`.
    static RefdataBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace bpt::refdata::messaging
