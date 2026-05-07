#include "refdata/messaging/aeron_bus.h"

#include "refdata/config/settings.h"
#include "refdata/messaging/fee_schedule_publisher.h"
#include "refdata/messaging/refdata_control_subscriber.h"
#include "refdata/messaging/refdata_delta_publisher.h"
#include "refdata/messaging/refdata_snapshot_publisher.h"
#include "refdata/messaging/refdata_status_publisher.h"

namespace bpt::refdata::messaging {

AeronBus AeronBus::build(std::shared_ptr<aeron::Aeron> aeron,
                         const config::Settings& settings) {
    AeronBus bus;
    bus.control_source = std::make_unique<RefdataControlSubscriber>(
        aeron, settings.control.channel, settings.control.stream_id);
    bus.snapshot_sink = std::make_unique<RefdataSnapshotPublisher>(
        aeron, settings.snapshot.channel, settings.snapshot.stream_id);
    bus.delta_sink = std::make_shared<RefdataDeltaPublisher>(
        aeron, settings.delta.channel, settings.delta.stream_id);
    bus.fee_sink = std::make_shared<FeeSchedulePublisher>(
        aeron, settings.fee_schedule.channel, settings.fee_schedule.stream_id);
    bus.status_sink = std::make_shared<RefdataStatusPublisher>(
        aeron, settings.refdata_status.channel, settings.refdata_status.stream_id);
    return bus;
}

}  // namespace bpt::refdata::messaging
