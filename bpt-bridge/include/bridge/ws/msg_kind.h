#pragma once

#include <cstdint>

namespace bpt::bridge {

/// Tagged message kinds — the WsServer needs to know what kind of message
/// it is so it can update its session-replay snapshot correctly.
enum class MsgKind : uint8_t {
    Session,      ///< latest wins
    Status,       ///< latest wins
    Tick,         ///< latest wins (per symbol — single-symbol for now)
    Fill,         ///< appended to rolling buffer
    Position,     ///< latest wins (per symbol)
    Order,        ///< not snapshotted — transient lifecycle events
    Toxicity,     ///< latest wins — analytics toxicity scores
    MarketColor,  ///< latest wins (per underlying) — radar market-color snapshot
};

}  // namespace bpt::bridge
