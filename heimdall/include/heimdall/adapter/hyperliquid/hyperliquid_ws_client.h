#pragma once

// Persistent WebSocket client to the Hyperliquid endpoint — owns the
// connection, the read loop, the ping thread, and the request/response
// plumbing for HL's `{"method":"post"}` action API.
//
// Architectural notes:
//
//   - Lives inside HyperliquidOrderAdapter and shares the adapter's
//     io_context + ssl_context (OrderAdapterBase owns those and drives
//     the reconnect loop via connect_and_run).
//
//   - `run()` is called from the adapter's IO thread and blocks for the
//     duration of one connection session. It establishes the WS,
//     subscribes to userFills, spawns the ping thread, and runs the
//     read loop until either the stop flag is set or the connection
//     throws. On exit (normal or exception), the published stream is
//     cleared and any pending post futures are failed so senders never
//     hang on a dead connection.
//
//   - `post_action()` is called from the OrderProcessor thread (or the
//     detached account-snapshot fetch thread) and serialises a signed
//     action into the `{"method":"post","id":<N>,...}` envelope, sends
//     it under the write mutex, and blocks on a std::promise until the
//     reader thread matches the response by id. Beast's websocket::stream
//     supports concurrent read+write across threads as long as each
//     direction is single-threaded, so the read loop and the post writer
//     run concurrently without stepping on each other.
//
//   - `channel:"user"` frames (userFills) are forwarded to the caller
//     via a UserFillsHandler set before run() so the adapter's exec
//     parser can still process them.
//
//   - `channel:"error"` frames carry no id, so they fail ALL in-flight
//     pending posts. Correct in the single-order case, but will need
//     revisiting once strategies pipeline concurrent requests.

#include "heimdall/adapter/hyperliquid/hyperliquid_signer.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json/fwd.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace heimdall::adapter::hyperliquid {

class HyperliquidWsClient {
public:
    // Called from the WS read-loop thread whenever a `channel:"user"`
    // frame arrives. The adapter wires this to its HyperliquidExecParser
    // to turn HL fills into ExecEvents.
    using UserFillsHandler = std::function<void(const boost::json::array& fills,
                                                 uint64_t recv_ns)>;

    HyperliquidWsClient(boost::asio::io_context& ioc,
                        boost::asio::ssl::context& ssl_ctx,
                        std::string host,
                        std::string port,
                        std::string path,
                        std::string wallet_address);

    // Must be called before run(). Callback stays live for the lifetime
    // of the client — captured references must outlive the WS thread.
    void set_user_fills_handler(UserFillsHandler h);

    // One connection session. Blocks on the read loop until stop_flag
    // goes true (clean shutdown) or the connection throws (triggers
    // reconnect in the OrderAdapterBase loop). Sets connected to true
    // after the handshake completes; base class clears it on return.
    void run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected);

    // Send a signed action over the WS post channel and block until
    // HL replies with the matching id or the 5 s timeout fires. Returns
    // the response payload body (same JSON shape the REST /exchange
    // used to return). Throws on timeout, write error, or if the WS
    // isn't currently connected.
    std::string post_action(const boost::json::value& action,
                            uint64_t nonce,
                            const SignedTransaction& sig);

private:
    void handle_frame(const std::string& payload, uint64_t recv_ns);
    void fail_pending_posts(const std::string& reason);

    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    const std::string host_;
    const std::string port_;
    const std::string path_;
    const std::string wallet_address_;

    using WsStream = boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::beast::tcp_stream>>;

    // lifecycle_mutex_ guards the ws_stream_ pointer; write_mutex_
    // serialises concurrent ws->write() calls from the ping thread +
    // post_action senders. Reads don't need either mutex — the read
    // loop uses a local shared_ptr captured at run() entry.
    std::mutex lifecycle_mutex_;
    std::mutex write_mutex_;
    std::shared_ptr<WsStream> stream_;

    std::atomic<uint64_t> next_post_id_{1};
    std::mutex pending_posts_mutex_;
    std::unordered_map<uint64_t, std::promise<std::string>> pending_posts_;

    UserFillsHandler user_fills_handler_;
};

}  // namespace heimdall::adapter::hyperliquid
