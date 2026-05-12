#pragma once

/// @file
/// Subscriber for the dashboard kill-switch / resume control channel.
/// 1-byte messages: 0x00 = HALT, 0x01 = RESUME. The bridge sends these
/// via Aeron when an operator clicks the dashboard button. Strategy
/// translates HALT into trading_halted_=true and stops sending orders.

#include <Aeron.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::strategy::messaging {

class DashboardControlSubscriber {
public:
    using OnCommandFn = std::function<void(uint8_t cmd)>;

    DashboardControlSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    /// True when the underlying subscription connected within the
    /// startup wait. False = subscription unavailable (logged at boot).
    [[nodiscard]] bool is_ready() const { return static_cast<bool>(sub_); }

    int poll(int fragment_limit = 1);

    OnCommandFn on_command;

private:
    std::shared_ptr<aeron::Subscription> sub_;
};

}  // namespace bpt::strategy::messaging
