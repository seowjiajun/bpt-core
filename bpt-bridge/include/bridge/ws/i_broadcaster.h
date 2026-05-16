#pragma once

/// @file
/// Port interface for the dashboard broadcast sink. BridgeService depends on
/// this rather than the concrete WsServer so tests can substitute a
/// FakeBroadcaster that records the (kind, payload) pairs without opening a
/// real TCP listener.
///
/// The shape mirrors WsServer's existing `publish(MsgKind, std::string)`
/// method one-to-one, so production wiring is a single inheritance line and
/// the rest of bridge_service.cpp stays unchanged.

#include "bridge/ws/msg_kind.h"

#include <functional>
#include <string>

namespace bpt::bridge::ws {

class IBroadcaster {
public:
    virtual ~IBroadcaster() = default;

    /// Broadcast a typed message to every connected dashboard client and
    /// update the session-replay snapshot. Thread-safe in concrete impls.
    virtual void publish(MsgKind kind, std::string message) = 0;

    /// Install the handler invoked when a dashboard client sends a command
    /// (e.g. "halt", "resume"). Concrete WsServer fires on its IO thread;
    /// fakes can call the handler synchronously to simulate dashboard input.
    /// Set to `nullptr` to detach before the handler's captured state goes
    /// away.
    using CommandHandler = std::function<void(const std::string& cmd)>;
    virtual void set_command_handler(CommandHandler h) = 0;
};

}  // namespace bpt::bridge::ws
