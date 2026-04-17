#pragma once

#include "md_gateway/md/md_types.h"

namespace bpt::md_gateway::messaging {

class IMdPublisher {
public:
    virtual ~IMdPublisher() = default;

    virtual void publish(const md::MdBbo& bbo) = 0;
    virtual void publish(const md::MdTrade& trade) = 0;
    virtual void publish(const md::MdOrderBook& book) = 0;
};

}  // namespace bpt::md_gateway::messaging
