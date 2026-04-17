#pragma once

// Persistent WebSocket client for Deribit's JSON-RPC endpoint. Owns
// one connection session's read loop and thread-safe send.
//
// Unlike OKX's ws_client, there is no ping thread — Deribit's
// heartbeat is application-level via `public/set_heartbeat` +
// `test_request` handled inside the adapter's message_handler, so
// the ws_client just does raw connect + login + read.
//
// Lifecycle: the client shares the adapter's io_context + ssl_context
// (OrderAdapterBase owns both and drives reconnect via connect_and_run).
// run() blocks for one session; on exit — clean or thrown — the send
// callback is cleared so a concurrent send() call sees disconnected
// state and returns false.

#include "order_gateway/config/settings.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace bpt::order_gateway::adapter::deribit {

class DeribitWsClient {
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
    // right after handshake (before the login reply arrives).
    void run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected);

    // Thread-safe frame send. Returns false if not currently connected.
    // Throws only on raw write error — callers treat a thrown send as
    // a disconnect.
    bool send(const std::string& frame);

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    const config::AdapterConfig& cfg_;
    MessageHandler message_handler_;
    LoginMsgBuilder login_msg_builder_;

    mutable std::mutex send_mu_;
    std::function<void(const std::string&)> ws_send_;
};

}  // namespace bpt::order_gateway::adapter::deribit
