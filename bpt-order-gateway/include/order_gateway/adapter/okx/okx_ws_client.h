#pragma once

// Persistent WebSocket client for the OKX private endpoint — owns one
// connection session's read loop, ping cadence, and thread-safe send.
//
// Unlike Hyperliquid's ws_client, OKX's order API is fire-and-forget:
// there is no request/response id matching at this layer. Order-place
// acks arrive asynchronously through the `orders` channel push and are
// routed to OKXExecParser by the adapter's message_handler — so this
// client deliberately does not carry any pending-posts / promise-future
// plumbing. It is just connect + login + read loop + send.
//
// Lifecycle: the client shares the adapter's io_context + ssl_context
// (OrderAdapterBase owns both and drives the reconnect loop via
// connect_and_run). run() blocks for one session; on exit — clean or
// thrown — the send callback is cleared so any concurrent send() call
// sees the disconnected state and returns false.

#include "order_gateway/config/settings.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace bpt::order_gateway::adapter::okx {

class OKXWsClient {
public:
    // Called from the read-loop thread for every non-ping/pong frame.
    // recv_ns is WallClock wall-time ns captured right after read().
    using MessageHandler = std::function<void(const std::string& payload, uint64_t recv_ns)>;

    // Produces the OKX `{"op":"login",...}` envelope. Called once per
    // session, right after the WS handshake completes, so the signature
    // inside is signed with a fresh timestamp.
    using LoginMsgBuilder = std::function<std::string()>;

    OKXWsClient(boost::asio::io_context& ioc,
                boost::asio::ssl::context& ssl_ctx,
                const config::AdapterConfig& cfg);

    // Both must be set before run(). References captured in the handlers
    // must outlive the WS thread.
    void set_message_handler(MessageHandler h);
    void set_login_msg_builder(LoginMsgBuilder b);

    // One connection session. Blocks on the read loop until stop_flag
    // goes true (clean shutdown) or the connection throws (triggers the
    // base-class reconnect). Sets `connected` to true once the handshake
    // completes; OrderAdapterBase clears it on return.
    void run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected);

    // Thread-safe frame send. Returns false if the WS is not currently
    // connected (caller should emit a rejection). Throws only on a raw
    // write error — callers treat a thrown send as a disconnect.
    bool send(const std::string& frame);

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    const config::AdapterConfig& cfg_;
    MessageHandler message_handler_;
    LoginMsgBuilder login_msg_builder_;

    // ws_send_ captures a reference to the ws stream owned by run()'s
    // stack frame. Cleared on run() exit so send() can detect the
    // disconnected state. send_mu_ also serialises concurrent writes
    // from multiple OrderProcessor callers against the ping writer.
    mutable std::mutex send_mu_;
    std::function<void(const std::string&)> ws_send_;
};

}  // namespace bpt::order_gateway::adapter::okx
