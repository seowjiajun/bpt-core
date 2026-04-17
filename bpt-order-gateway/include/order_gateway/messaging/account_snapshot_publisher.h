#pragma once

#include "order_gateway/adapter/common/account_snapshot_data.h"

#include <Aeron.h>

#include <memory>
#include <mutex>
#include <string>

namespace bpt::order_gateway::messaging {

// Publishes AccountSnapshot (SBE id=27) to Strategy on stream 3004.
// Thread-safe: publish() may be called from a detached fetch thread.
class AccountSnapshotPublisher {
public:
    AccountSnapshotPublisher(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const adapter::AccountSnapshotData& snapshot);

private:
    std::shared_ptr<aeron::Publication> publication_;
    std::mutex mutex_;  // Aeron publication is not thread-safe; guard concurrent publish() calls.
};

}  // namespace bpt::order_gateway::messaging
