#pragma once

/// \file
/// \brief Composition root that wires the Aeron-backed implementations of the messaging ports.
///
/// OrderGatewayService talks to the four messaging ports (api::OrderSubscriber +
/// api::ExecReportPublisher + api::AccountSnapshotPublisher + api::HeartbeatPublisher)
/// without knowing how they are implemented. `OrderGatewayAeronBus::build()` is
/// the single place that constructs the Aeron-backed concrete classes
/// and bundles them into a struct the app accepts in its constructor —
/// swap this factory for a different one (e.g. an in-memory bus for
/// seam tests) and the app code is unchanged.
///
/// Mirrors the bpt-md-gateway and bpt-refdata bus shape.
///
/// Lifetime: OrderGatewayBus owns the publisher and subscriber objects but
/// hands ownership to OrderGatewayService at construction; OrderGatewayBus itself
/// is a value type that can be moved out at the wiring site in main.cpp.

#include "order_gateway/messaging/publishers/api/account_snapshot_publisher.h"
#include "order_gateway/messaging/publishers/api/exec_report_publisher.h"
#include "order_gateway/messaging/publishers/api/heartbeat_publisher.h"
#include "order_gateway/messaging/subscribers/api/order_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::order_gateway::config {
struct Settings;
}

namespace bpt::order_gateway::messaging {

/// \brief Bundle of messaging-port implementations handed to OrderGatewayService.
///
/// All four ports are exposed via interface type so alternate
/// implementations (test fakes, replay drivers) can substitute without
/// rebuilding the app.
struct OrderGatewayBus {
    /// \brief Inbound: NewOrder / Cancel / CancelAll / Modify /
    ///        AccountSnapshotRequest fragments from strategy.
    ///
    /// Polled from OrderGatewayService::run(); each fragment dispatched to
    /// the matching OrderProcessor handler via the public callbacks.
    std::shared_ptr<api::OrderSubscriber> control_sub;

    /// \brief Outbound: ExecutionReport on the exec-report stream.
    ///
    /// Driven by OrderProcessor on every exec event from a venue adapter
    /// plus synthetic events (risk rejects, stale-order cancellations).
    std::shared_ptr<api::ExecReportPublisher> exec_pub;

    /// \brief Outbound: AccountSnapshot on its own stream.
    ///
    /// Driven by detached worker threads spawned per
    /// AccountSnapshotRequest (REST fetches are blocking and must not
    /// run on the poll thread) plus a periodic 5 s republish from the
    /// main loop.
    std::shared_ptr<api::AccountSnapshotPublisher> account_snapshot_pub;

    /// \brief Outbound: OrderGatewayHeartbeat on a fixed cadence.
    std::shared_ptr<api::HeartbeatPublisher> heartbeat_pub;

};

class OrderGatewayAeronBus {
public:
    /// \brief Construct the prod (Aeron-backed) implementations of all
    ///        four ports.
    ///
    /// Reads channel + stream-id assignments from `settings.aeron`. The
    /// supplied `aeron` shared client must already have a MediaDriver
    /// connection — see bpt::app::run() which sets it up.
    static OrderGatewayBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace bpt::order_gateway::messaging
