#pragma once

#include "refdata/messaging/messages.h"
#include "refdata/messaging/subscribers/api/refdata_control_subscriber.h"

#include <Aeron.h>

#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/aeron/subscriber.h>
#include <memory>

namespace bpt::refdata::messaging::aeron {

class RefdataControlSubscriber final : public api::RefdataControlSubscriber {
public:
    RefdataControlSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    // Returns number of fragments processed (0 = idle, use for idle strategy).
    int poll(RequestHandler handler) override;

private:
    std::unique_ptr<bpt::common::aeron::Subscriber> subscription_;
    RequestHandler current_handler_;
};

}  // namespace bpt::refdata::messaging::aeron
