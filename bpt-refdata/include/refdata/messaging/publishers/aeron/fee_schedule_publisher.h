#pragma once

#include "refdata/messaging/codecs/sbe_fee_schedule_codec.h"
#include "refdata/messaging/publishers/api/fee_schedule_publisher.h"
#include "refdata/model/fee_schedule.h"

#include <Aeron.h>

#include <bpt_common/aeron/stream_config.h>
#include <memory>

namespace bpt::refdata::messaging::aeron {

// Publishes FeeSchedule SBE messages (template id=19) on stream 1004.
class FeeSchedulePublisher final : public api::FeeSchedulePublisher {
public:
    FeeSchedulePublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    void publish(const model::FeeScheduleState& fs) override;

private:
    std::shared_ptr<::aeron::Publication> publication_;
    SbeFeeScheduleCodec codec_;
};

}  // namespace bpt::refdata::messaging::aeron
