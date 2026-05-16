#pragma once

#include "order_gateway/messaging/codecs/sbe_account_snapshot_codec.h"
#include "order_gateway/messaging/publishers/api/account_snapshot_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <memory>
#include <string>

namespace bpt::order_gateway::messaging::aeron {

/// \brief Aeron-backed concrete for api::AccountSnapshotPublisher.
///
/// Publishes AccountSnapshot (SBE id=27) to Strategy on stream 3004.
/// Thread-safety comes from the underlying bpt::common::aeron::Publisher
/// — adapter worker threads may publish concurrently after a successful
/// REST fetch.
class AccountSnapshotPublisher final : public api::AccountSnapshotPublisher {
public:
    AccountSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const adapter::AccountSnapshotData& snapshot) override;

private:
    bpt::common::aeron::Publisher publisher_;
    SbeAccountSnapshotCodec       codec_;
};

}  // namespace bpt::order_gateway::messaging::aeron
