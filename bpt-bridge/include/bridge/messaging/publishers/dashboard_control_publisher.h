#pragma once

/// @file
/// Aeron-backed implementation of IDashboardControlSink. Publishes single-byte
/// HALT (0x00) / RESUME (0x01) commands on the bridge → strategy control
/// stream. Constructed by BridgeAeronBus::build() at the prod composition
/// root and held inside the BridgeBus.

#include "bridge/messaging/publishers/i_dashboard_control_sink.h"

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bpt::bridge::messaging {

class DashboardControlPublisher : public IDashboardControlSink {
public:
    DashboardControlPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                              const std::string& channel,
                              int32_t stream_id);

    void publish_halt() override;
    void publish_resume() override;

private:
    void publish_byte(uint8_t cmd);

    std::shared_ptr<::aeron::Publication> pub_;
};

}  // namespace bpt::bridge::messaging
