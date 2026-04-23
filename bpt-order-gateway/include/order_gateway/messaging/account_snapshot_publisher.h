#pragma once

#include "order_gateway/adapter/common/account_snapshot_data.h"

#include <Aeron.h>

#include <memory>
#include <string>
#include <bpt_common/aeron/publisher.h>

namespace bpt::order_gateway::messaging {

// Publishes AccountSnapshot (SBE id=27) to Strategy on stream 3004.
// Thread-safety comes from the underlying bpt::common::aeron::Publisher.
class AccountSnapshotPublisher {
public:
    AccountSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const adapter::AccountSnapshotData& snapshot);

private:
    bpt::common::aeron::Publisher publisher_;
};

}  // namespace bpt::order_gateway::messaging
