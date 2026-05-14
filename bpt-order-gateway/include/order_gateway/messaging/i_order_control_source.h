#pragma once

/// \file
/// \brief Inbound port: control fragments from strategy on the order channel.
///
/// Drains five SBE message types off Aeron stream 3001:
/// NewOrder, CancelOrder, CancelAll, ModifyOrder, AccountSnapshotRequest.
/// Each fragment is dispatched to the appropriate user-supplied
/// callback. The poll() loop is driven from the main poll thread
/// inside OrderGatewayService::run().
///
/// Implementations: OrderSubscriber (Aeron-backed) in prod; a fake
/// implementation can drive the five message types directly into the
/// callbacks for seam testing without an Aeron MediaDriver.

#include <messages/AccountSnapshotRequest.h>
#include <messages/CancelAll.h>
#include <messages/CancelOrder.h>
#include <messages/ModifyOrder.h>
#include <messages/NewOrder.h>

#include <functional>

namespace bpt::order_gateway::messaging {

using OnNewOrderFn = std::function<void(const bpt::messages::NewOrder&)>;
using OnCancelFn = std::function<void(const bpt::messages::CancelOrder&)>;
using OnCancelAllFn = std::function<void(const bpt::messages::CancelAll&)>;
using OnModifyFn = std::function<void(const bpt::messages::ModifyOrder&)>;
using OnAccountSnapshotRequestFn = std::function<void(const bpt::messages::AccountSnapshotRequest&)>;

/// \brief Contract for the inbound order-control source.
///
/// Single-threaded: poll() is called from OrderGatewayService's main loop
/// only. Implementations need not be thread-safe.
///
/// Callbacks are public std::function members rather than virtual
/// setters to match the existing OrderSubscriber surface. They must be
/// set before the first poll() call; references handed to the
/// callbacks are only valid for the duration of the call.
class IOrderControlSource {
public:
    virtual ~IOrderControlSource() = default;

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

}  // namespace bpt::order_gateway::messaging
