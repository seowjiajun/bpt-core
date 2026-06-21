#pragma once

/// @file
/// Aeron+SBE implementation of api::BalanceSnapshotPublisher. Composes
/// SbeBalanceSnapshotCodec for serialisation.

#include "pms/messaging/codecs/sbe_balance_snapshot_codec.h"
#include "pms/messaging/publishers/api/balance_snapshot_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/stream_config.h>
#include <memory>

namespace bpt::pms::messaging::aeron {

class BalanceSnapshotPublisher : public api::BalanceSnapshotPublisher {
public:
    BalanceSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    void publish(const adapter::BalanceSnapshot& snapshot) override;

private:
    bpt::common::aeron::Publisher publisher_;
    SbeBalanceSnapshotCodec codec_;
};

}  // namespace bpt::pms::messaging::aeron
