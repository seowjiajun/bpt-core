#pragma once

/// @file
/// Subscriber for the bpt-analytics toxicity stream. Pulls
/// `ToxicityUpdate` POD messages off Aeron and dispatches them via a
/// std::function callback set by the strategy. Same shape as the other
/// strategy-side clients (MdClient, VolSurfaceClient): owns its
/// Subscription, exposes poll().

#include <Aeron.h>

#include <analytics/messaging/toxicity_update.h>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

namespace bpt::strategy::messaging {

class ToxicitySubscriber {
public:
    using OnUpdateFn = std::function<void(const bpt::analytics::messaging::ToxicityUpdate&)>;

    ToxicitySubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    int poll(int fragment_limit = 4);

    OnUpdateFn on_update;

private:
    std::shared_ptr<aeron::Subscription> sub_;
};

}  // namespace bpt::strategy::messaging
