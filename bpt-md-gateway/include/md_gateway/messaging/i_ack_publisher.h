#pragma once

#include <messages/AckStatus.h>

#include <cstdint>

namespace bpt::md_gateway::messaging {

class IAckPublisher {
public:
    virtual ~IAckPublisher() = default;

    virtual void publish_ack(uint64_t correlation_id,
                             uint64_t instrument_id,
                             const char* exchange,
                             bpt::messages::AckStatus::Value status) = 0;

    virtual void publish_subscription_heartbeat(uint64_t instrument_id) = 0;

    virtual void publish_service_heartbeat() = 0;
};

}  // namespace bpt::md_gateway::messaging
