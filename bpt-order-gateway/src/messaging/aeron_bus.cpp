/// \file
/// \brief AeronBus::build — instantiate the prod messaging-port concrete classes.
///
/// One place that maps `settings.aeron.*` channel+stream pairs onto the
/// concrete Aeron-backed classes. Kept tiny on purpose — main.cpp's
/// wiring is a single call (`AeronBus::build`) and the rest of the
/// binary never has to look at it again.

#include "order_gateway/messaging/aeron_bus.h"

#include "order_gateway/config/settings.h"
#include "order_gateway/messaging/account_snapshot_publisher.h"
#include "order_gateway/messaging/exec_report_publisher.h"
#include "order_gateway/messaging/heartbeat_publisher.h"
#include "order_gateway/messaging/order_subscriber.h"

namespace bpt::order_gateway::messaging {

AeronBus AeronBus::build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings) {
    AeronBus bus;
    bus.control_source =
        std::make_shared<OrderSubscriber>(aeron, settings.aeron.order.channel, settings.aeron.order.stream_id);
    bus.exec_sink = std::make_shared<ExecReportPublisher>(aeron,
                                                          settings.aeron.exec_report.channel,
                                                          settings.aeron.exec_report.stream_id);
    bus.account_snapshot_sink = std::make_shared<AccountSnapshotPublisher>(aeron,
                                                                           settings.aeron.account_snapshot.channel,
                                                                           settings.aeron.account_snapshot.stream_id);
    bus.heartbeat_sink = std::make_shared<HeartbeatPublisher>(aeron,
                                                              settings.aeron.heartbeat.channel,
                                                              settings.aeron.heartbeat.stream_id);
    return bus;
}

}  // namespace bpt::order_gateway::messaging
