#pragma once

#include "md_gateway/messaging/i_ack_publisher.h"

#include <messages/AckStatus.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bpt::md_gateway::test {

struct AckEvent {
    uint64_t correlation_id{};
    uint64_t instrument_id{};
    std::string exchange;
    bpt::messages::AckStatus::Value status{};
};

class FakeAckPublisher : public messaging::IAckPublisher {
public:
    void publish_ack(uint64_t correlation_id,
                     uint64_t instrument_id,
                     const char* exchange,
                     bpt::messages::AckStatus::Value status) override {
        acks.push_back(AckEvent{correlation_id, instrument_id, exchange, status});
    }

    void publish_subscription_heartbeat(uint64_t instrument_id) override {
        subscription_heartbeats.push_back(instrument_id);
    }

    void publish_service_heartbeat() override { service_heartbeat_count++; }

    void reset() {
        acks.clear();
        subscription_heartbeats.clear();
        service_heartbeat_count = 0;
    }

    std::vector<AckEvent> acks;
    std::vector<uint64_t> subscription_heartbeats;
    int service_heartbeat_count{0};
};

}  // namespace bpt::md_gateway::test
