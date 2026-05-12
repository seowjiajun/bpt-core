#pragma once

/// \file
/// \brief Binance MD WebSocket client — RunLoop subclass owned by BinanceMdAdapter.

#include <bpt_common/ws/run_loop.h>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace bpt::md_gateway::adapter {

/// \brief Drives the Binance MD WS read/send/ping loop.
///
/// Binance has no application-level pings (Beast control-frame pings
/// suffice) and no on_tick work (subscriptions are baked into the URL),
/// so on_frame is the only override — it just forwards every frame to
/// the adapter's handler.
class BinanceMdWsClient : public bpt::common::ws::RunLoop {
public:
    using FrameHandler = std::function<void(std::string_view payload, uint64_t recv_ns)>;

    void set_frame_handler(FrameHandler h) { handler_ = std::move(h); }

protected:
    void on_frame(std::string_view payload, uint64_t recv_ns) override {
        if (handler_)
            handler_(payload, recv_ns);
    }

private:
    FrameHandler handler_;
};

}  // namespace bpt::md_gateway::adapter
