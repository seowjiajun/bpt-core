#pragma once

/// \file
/// \brief Outbound port: AccountSnapshot publish toward strategy / dashboard.
///
/// AccountSnapshot is the periodic + on-demand picture of the account
/// state at a venue (positions, currency balances, available margin).
/// Published on its own Aeron stream so subscribers (strategy gating
/// shutdown_flatten, dashboard HoldingsPanel) can attach without
/// touching the higher-rate exec_report stream.
///
/// Implementations: aeron::AccountSnapshotPublisher in prod; fakes in
/// unit tests if/when needed.

#include "order_gateway/adapter/common/account_snapshot_data.h"

namespace bpt::order_gateway::messaging::api {

/// \brief Contract for the AccountSnapshot outbound port.
///
/// Called from a detached worker thread spawned per
/// AccountSnapshotRequest (REST fetch is blocking — must not run on
/// the poll loop). Implementations must be thread-safe for concurrent
/// publish() across worker threads from different adapters.
class AccountSnapshotPublisher {
public:
    virtual ~AccountSnapshotPublisher() = default;

    /// \brief Encode and publish one snapshot. May log + drop on
    ///        no-subscriber rather than block.
    virtual void publish(const adapter::AccountSnapshotData& snapshot) = 0;
};

}  // namespace bpt::order_gateway::messaging::api
