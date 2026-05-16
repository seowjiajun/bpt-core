#pragma once

#include "refdata/messaging/messages.h"
#include "refdata/messaging/streams.h"
#include "refdata/port/i_refdata_control_source.h"

#include <Aeron.h>

#include <bpt_common/aeron/subscriber.h>
#include <memory>

namespace bpt::refdata::messaging {

class RefdataControlSubscriber final : public port::IRefdataControlSource {
public:
    RefdataControlSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    // Returns number of fragments processed (0 = idle, use for idle strategy).
    int poll(RequestHandler handler) override;

private:
    std::unique_ptr<bpt::common::aeron::Subscriber> subscription_;
    RequestHandler current_handler_;
};

}  // namespace bpt::refdata::messaging
