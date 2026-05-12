#pragma once

/// \file
/// \brief Persistent WebSocket client for the OKX private endpoint.
///
/// Owns one connection session's read loop, ping cadence, and
/// thread-safe send. Inherits the connect/read/send/ping scaffolding
/// from `bpt::common::ws::RunLoop`; this class only supplies the
/// OKX-specific bits:
///   - build & send the `{"op":"login",...}` envelope on handshake.
///   - intercept raw "ping"/"pong" text frames (OKX's
///     application-level heartbeat) and auto-reply without waking the
///     adapter's message handler.
///   - forward every other frame to the adapter's message_handler.
///   - emit a 10-second text-ping heartbeat so OKX doesn't close the
///     connection at its 30 s idle threshold.
///
/// Unlike Hyperliquid's ws_client, OKX's order API is fire-and-forget:
/// there's no request/response id matching at this layer. Order acks
/// arrive asynchronously via the `orders` channel push and are routed
/// through the adapter's message_handler, so this class carries no
/// pending-posts / promise-future plumbing.

#include "order_gateway/config/settings.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <bpt_common/ws/run_loop.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace bpt::order_gateway::adapter::okx {

class OKXWsClient : public bpt::common::ws::RunLoop {
public:
    using MessageHandler = std::function<void(const std::string& payload, uint64_t recv_ns)>;
    using LoginMsgBuilder = std::function<std::string()>;

    OKXWsClient(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_ctx, const config::AdapterConfig& cfg);

    void set_message_handler(MessageHandler h);
    void set_login_msg_builder(LoginMsgBuilder b);

    void run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected);

protected:
    void on_handshake_complete() override;
    void on_frame(std::string_view payload, uint64_t recv_ns) override;
    std::optional<bpt::common::ws::PingConfig> ping_config() const override;

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    const config::AdapterConfig& cfg_;
    MessageHandler message_handler_;
    LoginMsgBuilder login_msg_builder_;
};

}  // namespace bpt::order_gateway::adapter::okx
