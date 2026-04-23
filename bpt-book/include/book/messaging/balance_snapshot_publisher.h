#pragma once

#include "book/adapter/balance_row.h"

#include <Aeron.h>

#include <memory>
#include <string>
#include <bpt_common/aeron/publisher.h>

namespace bpt::book::messaging {

class BalanceSnapshotPublisher {
public:
    BalanceSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                             const std::string& channel,
                             int stream_id);

    void publish(const adapter::BalanceSnapshot& snapshot);

private:
    bpt::common::aeron::Publisher publisher_;
};

}  // namespace bpt::book::messaging
