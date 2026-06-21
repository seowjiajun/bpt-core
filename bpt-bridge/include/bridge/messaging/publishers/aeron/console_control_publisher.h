#pragma once

/// @file
/// Aeron-backed implementation of api::ConsoleControlPublisher. Publishes
/// single-byte HALT (0x00) / RESUME (0x01) commands on the bridge → strategy
/// control stream. Constructed by BridgeAeronBus::build() at the prod
/// composition root and held inside the BridgeBus.

#include "bridge/messaging/publishers/api/console_control_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::bridge::messaging::aeron {

class ConsoleControlPublisher : public api::ConsoleControlPublisher {
public:
    ConsoleControlPublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    void publish_halt() override;
    void publish_resume() override;

private:
    void publish_byte(uint8_t cmd);

    std::shared_ptr<::aeron::Publication> pub_;
};

}  // namespace bpt::bridge::messaging::aeron
