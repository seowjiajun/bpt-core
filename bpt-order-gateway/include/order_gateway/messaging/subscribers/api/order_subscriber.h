#pragma once

/// \file
/// \brief Inbound port: control fragments from strategy on the order channel.
///
/// Drains five SBE message types off Aeron stream 3001:
/// NewOrder, CancelOrder, CancelAll, ModifyOrder, AccountSnapshotRequest.
/// SBE is decoded at the Aeron boundary; callbacks receive domain event types.
/// The poll() loop is driven from the main poll thread inside OrderGatewayService::run().
///
/// Implementations: aeron::OrderSubscriber in prod; a fake
/// implementation can drive the five message types directly into the
/// callbacks for seam testing without an Aeron MediaDriver.

#include "order_gateway/order/inbound_order_events.h"

#include <messages/AccountSnapshotRequest.h>

#include <functional>

namespace bpt::order_gateway::messaging {

using OnNewOrderFn = std::function<void(const order::NewOrderEvent&)>;
using OnCancelFn = std::function<void(const order::CancelOrderEvent&)>;
using OnCancelAllFn = std::function<void(const order::CancelAllEvent&)>;
using OnModifyFn = std::function<void(const order::ModifyOrderEvent&)>;
using OnAccountSnapshotRequestFn = std::function<void(const bpt::messages::AccountSnapshotRequest&)>;

namespace api {

/// \brief Contract for the inbound order-control source.
///
/// Single-threaded: poll() is called from OrderGatewayService's main loop
/// only. Implementations need not be thread-safe.
///
/// Callbacks are public std::function members rather than virtual
/// setters to match the existing OrderSubscriber surface. They must be
/// set before the first poll() call; references handed to the
/// callbacks are only valid for the duration of the call.
class OrderSubscriber {
public:
    virtual ~OrderSubscriber() = default;

    /// \brief Drain up to `fragment_limit` control fragments, dispatching
    ///        each to the matching handler.
    /// \return Number of fragments processed; 0 means idle.
    virtual int poll(int fragment_limit = 10) = 0;

    /// \name Per-message-type handlers — set before poll().
    /// \{
    OnNewOrderFn on_new_order;
    OnCancelFn on_cancel;
    OnCancelAllFn on_cancel_all;
    OnModifyFn on_modify;
    OnAccountSnapshotRequestFn on_account_snapshot_request;
    /// \}
};

}  // namespace api

}  // namespace bpt::order_gateway::messaging
