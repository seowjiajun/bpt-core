#pragma once

#include "refdata/messaging/publishers/api/refdata_delta_publisher.h"
#include "refdata/model/instrument.h"

#include <Aeron.h>

#include <messages/DeltaUpdateType.h>

#include <bpt_common/aeron/stream_config.h>
#include <memory>

namespace bpt::refdata::messaging::aeron {

class RefdataDeltaPublisher final : public api::RefdataDeltaPublisher {
public:
    RefdataDeltaPublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    void publish_delta(bpt::messages::DeltaUpdateType::Value update_type, const model::Instrument& inst) override;

    // Publish a heartbeat on the delta stream so subscribers can detect silent failures.
    // Uses DeltaUpdateType::NULL_VALUE with instrumentId=0; sequence number increments
    // so gap detection works uniformly across real deltas and heartbeats.
    void publish_heartbeat() override;

    uint64_t current_sequence() const override { return seq_; }

private:
    std::shared_ptr<::aeron::Publication> publication_;
    uint64_t seq_ = 0;
};

}  // namespace bpt::refdata::messaging::aeron
