#pragma once

#include "refdata/messaging/messages.h"
#include "refdata/messaging/streams.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <functional>
#include <memory>

namespace bpt::refdata::messaging {

class RefdataControlSubscriber {
public:
    using RequestHandler = std::function<void(const RefdataRequest&)>;

    RefdataControlSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    // Returns number of fragments processed (0 = idle, use for idle strategy).
    int poll(RequestHandler handler);

private:
    std::shared_ptr<aeron::Subscription> subscription_;
    std::shared_ptr<aeron::FragmentAssembler> assembler_;
    RequestHandler current_handler_;
};

}  // namespace bpt::refdata::messaging
