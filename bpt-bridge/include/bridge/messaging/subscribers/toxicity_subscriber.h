#pragma once

/// @file
/// Subscribes to bpt-analytics' toxicity stream and dispatches the POD
/// ToxicityUpdate to a user-supplied callback.

#include <analytics/messaging/toxicity_update.h>

#include <Aeron.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::bridge::messaging {

class ToxicitySubscriber {
public:
    using Handler = std::function<void(const bpt::analytics::messaging::ToxicityUpdate&)>;

    ToxicitySubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    void set_handler(Handler h) { handler_ = std::move(h); }

    int poll(int fragment_limit = 4);

private:
    std::shared_ptr<::aeron::Subscription> sub_;
    Handler handler_;
};

}  // namespace bpt::bridge::messaging
