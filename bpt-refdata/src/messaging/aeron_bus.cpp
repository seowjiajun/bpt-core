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
    bus.control_sub = std::make_unique<aeron::RefdataControlSubscriber>(aeron,
                                                                           settings.refdata_control.channel,
                                                                           settings.refdata_control.stream_id);
    bus.snapshot_pub = std::make_unique<aeron::RefdataSnapshotPublisher>(aeron,
                                                                          settings.refdata_snapshot.channel,
                                                                          settings.refdata_snapshot.stream_id);
    bus.delta_pub = std::make_shared<aeron::RefdataDeltaPublisher>(aeron,
                                                                    settings.refdata_delta.channel,
                                                                    settings.refdata_delta.stream_id);
    bus.fee_pub = std::make_shared<aeron::FeeSchedulePublisher>(aeron,
                                                                 settings.fee_schedule.channel,
                                                                 settings.fee_schedule.stream_id);
    bus.status_pub = std::make_shared<aeron::RefdataStatusPublisher>(aeron,
                                                                      settings.refdata_status.channel,
                                                                      settings.refdata_status.stream_id);
    return bus;
}

}  // namespace bpt::refdata::messaging
