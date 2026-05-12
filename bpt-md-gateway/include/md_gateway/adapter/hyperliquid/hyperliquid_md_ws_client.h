#pragma once

/// \file
/// \brief Hyperliquid MD WebSocket client — RunLoop subclass owned by HyperliquidMdAdapter.

#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/config/settings.h"

#include <bpt_common/ws/run_loop.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace bpt::md_gateway::adapter {

/// \brief Drives the Hyperliquid MD WS read/send/ping loop.
///
///   - on_frame forwards every frame to the adapter's handler.
///   - on_tick drains pending subscriptions across HL's 3 sub-types
///     (l2Book, trades, activeAssetCtx).
///   - ping_config emits HL's application-level `{"method":"ping"}` on
///     a 20 s cadence — HL closes idle sockets ~60 s after the last
///     client-sent message.
class HyperliquidMdWsClient : public bpt::common::ws::RunLoop {
public:
    using FrameHandler = std::function<void(std::string_view payload, uint64_t recv_ns)>;

    HyperliquidMdWsClient(const config::AdapterConfig& cfg, SubscriptionMap& subs) : cfg_(cfg), subs_(subs) {}

    void set_frame_handler(FrameHandler h) { handler_ = std::move(h); }

protected:
    void on_frame(std::string_view payload, uint64_t recv_ns) override {
        if (handler_)
            handler_(payload, recv_ns);
    }
    void on_tick() override;
    std::optional<bpt::common::ws::PingConfig> ping_config() const override;

private:
    const config::AdapterConfig& cfg_;
    SubscriptionMap& subs_;
    FrameHandler handler_;
};

}  // namespace bpt::md_gateway::adapter
