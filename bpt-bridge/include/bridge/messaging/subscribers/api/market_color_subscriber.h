#pragma once

/// @file
/// Port: bpt-radar MarketColor subscriber.

#include <radar/messaging/market_color.h>

#include <functional>

namespace bpt::bridge::messaging::api {

class MarketColorSubscriber {
public:
    using Handler = std::function<void(const bpt::radar::messaging::MarketColor&)>;

    virtual ~MarketColorSubscriber() = default;

    void set_handler(Handler h) { handler_ = std::move(h); }

    virtual int poll(int fragment_limit = 4) = 0;

protected:
    Handler handler_;
};

}  // namespace bpt::bridge::messaging::api
