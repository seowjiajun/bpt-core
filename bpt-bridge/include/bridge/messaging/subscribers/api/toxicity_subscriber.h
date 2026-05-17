#pragma once

/// @file
/// Port: bpt-analytics ToxicityUpdate subscriber.

#include <analytics/messaging/toxicity_update.h>

#include <functional>

namespace bpt::bridge::messaging::api {

class ToxicitySubscriber {
public:
    using Handler = std::function<void(const bpt::analytics::messaging::ToxicityUpdate&)>;

    virtual ~ToxicitySubscriber() = default;

    void set_handler(Handler h) { handler_ = std::move(h); }

    virtual int poll(int fragment_limit = 4) = 0;

protected:
    Handler handler_;
};

}  // namespace bpt::bridge::messaging::api
