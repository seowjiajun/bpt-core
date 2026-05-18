#pragma once

#include "refdata/messaging/codecs/sbe_fee_schedule_codec.h"
#include "refdata/messaging/publishers/api/fee_schedule_publisher.h"
#include "refdata/model/funding_rate.h"

#include <Aeron.h>

#include <memory>

namespace bpt::refdata::messaging::aeron {

// Publishes FeeSchedule SBE messages (template id=19) on stream 1004.
class FeeSchedulePublisher final : public api::FeeSchedulePublisher {
public:
    FeeSchedulePublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const model::FeeScheduleState& fs) override;

private:
    std::shared_ptr<::aeron::Publication> publication_;
    SbeFeeScheduleCodec                   codec_;
};

}  // namespace bpt::refdata::messaging::aeron
