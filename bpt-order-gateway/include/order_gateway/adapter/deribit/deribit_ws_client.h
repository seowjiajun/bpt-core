#pragma once

// Persistent WebSocket client for Deribit's JSON-RPC endpoint. Owns
// one connection session's read loop and thread-safe send.
//
// Inherits the connect/read/send scaffolding from bpt::common::ws::RunLoop;
// this class only implements the Deribit-specific bits:
//   - build & send the JSON-RPC `public/auth` message on handshake
//   - forward each inbound frame to the adapter's message_handler
//   - no ping thread — Deribit uses application-level set_heartbeat +
//     test_request handled inside the adapter's message_handler.

#include "order_gateway/config/settings.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <bpt_common/ws/run_loop.h>

namespace bpt::order_gateway::adapter::deribit {

class DeribitWsClient : public bpt::common::ws::RunLoop {
public:
    using MessageHandler = std::function<void(const std::string& payload, uint64_t recv_ns)>;
    using LoginMsgBuilder = std::function<std::string()>;

    DeribitWsClient(boost::asio::io_context& ioc,
                    boost::asio::ssl::context& ssl_ctx,
                    const config::AdapterConfig& cfg);

    // Both must be set before run(). References captured in the handlers
    // must outlive the WS thread.
    void set_message_handler(MessageHandler h);
    void set_login_msg_builder(LoginMsgBuilder b);

    // One connection session. Blocks on the read loop until stop_flag
    // goes true or the connection throws. Sets `connected` to true
    // right after handshake completes (before the auth reply arrives).
    void run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected);

protected:
    void on_handshake_complete() override;
    void on_frame(std::string_view payload, uint64_t recv_ns) override;

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    const config::AdapterConfig& cfg_;
    MessageHandler message_handler_;
    LoginMsgBuilder login_msg_builder_;
};

}  // namespace bpt::order_gateway::adapter::deribit
