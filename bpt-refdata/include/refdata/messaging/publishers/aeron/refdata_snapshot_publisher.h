#pragma once

#include "refdata/messaging/messages.h"
#include "refdata/messaging/publishers/api/refdata_snapshot_publisher.h"
#include "refdata/registry/instrument_registry.h"

#include <Aeron.h>

#include <bpt_common/aeron/stream_config.h>
#include <memory>

namespace bpt::refdata::messaging::aeron {

class RefdataSnapshotPublisher final : public api::RefdataSnapshotPublisher {
public:
    RefdataSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    void publish(const registry::InstrumentRegistry& registry,
                 const RefdataRequest& request,
                 uint64_t seq_start) override;

private:
    std::shared_ptr<::aeron::Publication> publication_;
};

}  // namespace bpt::refdata::messaging::aeron
