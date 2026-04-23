#pragma once

#include "book/adapter/balance_row.h"

#include <Aeron.h>

#include <memory>
#include <mutex>
#include <string>

namespace bpt::book::messaging {

class BalanceSnapshotPublisher {
public:
    BalanceSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                             const std::string& channel,
                             int stream_id);

    void publish(const adapter::BalanceSnapshot& snapshot);

private:
    std::shared_ptr<::aeron::Publication> publication_;
    std::mutex mutex_;
};

}  // namespace bpt::book::messaging
