#pragma once

#include "pms/adapter/balance_row.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <memory>
#include <string>

namespace bpt::pms::messaging {

class BalanceSnapshotPublisher {
public:
    BalanceSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const adapter::BalanceSnapshot& snapshot);

private:
    bpt::common::aeron::Publisher publisher_;
};

}  // namespace bpt::pms::messaging
