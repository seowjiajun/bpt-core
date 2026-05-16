#pragma once

#include "radar/messaging/market_color.h"

namespace bpt::radar::messaging::api {

class MarketColorPublisher {
public:
    virtual ~MarketColorPublisher() = default;
    virtual bool publish(const MarketColor& color) = 0;
};

}  // namespace bpt::radar::messaging::api
