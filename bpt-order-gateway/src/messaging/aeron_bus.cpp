/// \file
/// \brief OrderGatewayAeronBus::build — instantiate the prod messaging-port concrete classes.
///
/// One place that maps `settings.aeron.*` channel+stream pairs onto the
/// concrete Aeron-backed classes. Kept tiny on purpose — main.cpp's
/// wiring is a single call (`OrderGatewayAeronBus::build`) and the rest of the
/// binary never has to look at it again.

#include "order_gateway/messaging/aeron_bus.h"

#include "order_gateway/config/settings.h"
#include "order_gateway/messaging/publishers/aeron/account_snapshot_publisher.h"
#include "order_gateway/messaging/publishers/aeron/exec_report_publisher.h"
#include "order_gateway/messaging/publishers/aeron/heartbeat_publisher.h"
#include "order_gateway/messaging/subscribers/aeron/order_subscriber.h"

namespace bpt::order_gateway::messaging {

OrderGatewayBus OrderGatewayAeronBus::build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings) {
    OrderGatewayBus bus;
    bus.control_sub =
        std::make_shared<aeron::OrderSubscriber>(aeron, settings.aeron.order.channel, settings.aeron.order.stream_id);
    bus.exec_pub = std::make_shared<aeron::ExecReportPublisher>(aeron,
                                                                 settings.aeron.exec_report.channel,
                                                                 settings.aeron.exec_report.stream_id);
    bus.account_snapshot_pub = std::make_shared<aeron::AccountSnapshotPublisher>(
        aeron,
        settings.aeron.account_snapshot.channel,
        settings.aeron.account_snapshot.stream_id);
    bus.heartbeat_pub = std::make_shared<aeron::HeartbeatPublisher>(aeron,
                                                                     settings.aeron.heartbeat.channel,
                                                                     settings.aeron.heartbeat.stream_id);
    return bus;
}

}  // namespace bpt::order_gateway::messaging
