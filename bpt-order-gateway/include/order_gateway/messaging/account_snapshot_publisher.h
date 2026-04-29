#pragma once

#include "order_gateway/messaging/i_account_snapshot_publisher.h"

#include <Aeron.h>

#include <memory>
#include <string>
#include <bpt_common/aeron/publisher.h>

namespace bpt::order_gateway::messaging {

/// \brief Aeron-backed concrete for IAccountSnapshotPublisher.
///
/// Publishes AccountSnapshot (SBE id=27) to Strategy on stream 3004.
/// Thread-safety comes from the underlying bpt::common::aeron::Publisher
/// — adapter worker threads may publish concurrently after a successful
/// REST fetch.
class AccountSnapshotPublisher final : public IAccountSnapshotPublisher {
public:
    AccountSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                             const std::string& channel,
                             int stream_id);

    void publish(const adapter::AccountSnapshotData& snapshot) override;

private:
    bpt::common::aeron::Publisher publisher_;
};

}  // namespace bpt::order_gateway::messaging
