#pragma once

/// \file
/// \brief OKX MD WebSocket client — RunLoop subclass owned by OkxMdAdapter.

#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/config/settings.h"

#include <bpt_common/ws/run_loop.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace bpt::md_gateway::adapter {

/// \brief Drives the OKX MD WS read/send/ping loop.
///
/// Owns the venue WS protocol bits:
///   - on_frame intercepts text-frame "ping"/"pong" keepalives (replies
///     with "pong"; drops "pong" frames). Real frames go to the
///     adapter's frame handler.
///   - on_tick drains pending subscriptions added at runtime as a
///     fallback for the immediate-send path in the adapter.
///   - ping_config emits a text-frame "ping" on the configured cadence.
class OkxMdWsClient : public bpt::common::ws::RunLoop {
public:
    using FrameHandler = std::function<void(std::string_view payload, uint64_t recv_ns)>;

    OkxMdWsClient(const config::AdapterConfig& cfg, SubscriptionMap& subs) : cfg_(cfg), subs_(subs) {}

    void set_frame_handler(FrameHandler h) { handler_ = std::move(h); }

protected:
    void on_frame(std::string_view payload, uint64_t recv_ns) override;
    void on_tick() override;
    std::optional<bpt::common::ws::PingConfig> ping_config() const override;

private:
    const config::AdapterConfig& cfg_;
    SubscriptionMap& subs_;
    FrameHandler handler_;
};

}  // namespace bpt::md_gateway::adapter
