#include "refdata/messaging/aeron_bus.h"

#include "refdata/config/settings.h"
#include "refdata/messaging/publishers/aeron/fee_schedule_publisher.h"
#include "refdata/messaging/publishers/aeron/refdata_delta_publisher.h"
#include "refdata/messaging/publishers/aeron/refdata_snapshot_publisher.h"
#include "refdata/messaging/publishers/aeron/refdata_status_publisher.h"
#include "refdata/messaging/subscribers/aeron/refdata_control_subscriber.h"

namespace bpt::refdata::messaging {

RefdataBus RefdataAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    RefdataBus bus;
    bus.control_sub = std::make_unique<aeron::RefdataControlSubscriber>(aeron, settings.refdata_control);
    bus.snapshot_pub = std::make_unique<aeron::RefdataSnapshotPublisher>(aeron, settings.refdata_snapshot);
    bus.delta_pub = std::make_shared<aeron::RefdataDeltaPublisher>(aeron, settings.refdata_delta);
    bus.fee_pub = std::make_shared<aeron::FeeSchedulePublisher>(aeron, settings.fee_schedule);
    bus.status_pub = std::make_shared<aeron::RefdataStatusPublisher>(aeron, settings.refdata_status);
    return bus;
}

}  // namespace bpt::refdata::messaging
