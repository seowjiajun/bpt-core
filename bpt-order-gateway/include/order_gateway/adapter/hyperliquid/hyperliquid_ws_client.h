#pragma once

// Persistent WebSocket client to the Hyperliquid endpoint. Inherits
// the connect/read/send/ping scaffolding from bpt::common::ws::RunLoop and
// layers HL-specific request/response plumbing on top:
//
//   - Subscribes to `userFills` on handshake so the adapter's exec
//     parser sees every fill event.
//   - Provides post_action() — the HL-style "send a signed envelope,
//     block on the matching id, return the response body" pattern used
//     by every order-entry call.
//   - Forwards `channel:"error"` and stale-id responses by failing all
//     pending posts so senders never hang on a dead session.
//
// post_action() is called from external threads (OrderProcessor +
// detached account-snapshot fetcher). RunLoop::send() is thread-safe
// (single-writer mutex + single-reader cursor), so Beast's concurrent
// read/write invariant holds without any additional mutexes here.

#include "order_gateway/adapter/hyperliquid/hyperliquid_signer.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/json/fwd.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>
#include <bpt_common/ws/run_loop.h>

namespace bpt::order_gateway::adapter::hyperliquid {

class HyperliquidWsClient : public bpt::common::ws::RunLoop {
public:
    using UserFillsHandler = std::function<void(const boost::json::array& fills,
                                                 uint64_t recv_ns)>;

    HyperliquidWsClient(boost::asio::io_context& ioc,
                        boost::asio::ssl::context& ssl_ctx,
                        std::string host,
                        std::string port,
                        std::string path,
                        std::string wallet_address,
                        std::vector<std::string> pinned_tls_sha256 = {});

    void set_user_fills_handler(UserFillsHandler h);

    // One connection session: connect, subscribe, drive the read loop
    // until stop_flag or a WS error. Blocks on RunLoop::run internally.
    void run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected);

    // Send a signed action over the post channel and block until HL
    // replies with the matching id (5 s timeout). Throws on timeout,
    // write error, or disconnected WS. Thread-safe — called from the
    // OrderProcessor thread and the detached account-snapshot fetcher.
    std::string post_action(const boost::json::value& action,
                            uint64_t nonce,
                            const SignedTransaction& sig);

protected:
    void on_handshake_complete() override;
    void on_frame(std::string_view payload, uint64_t recv_ns) override;
    std::optional<bpt::common::ws::PingConfig> ping_config() const override;

private:
    void handle_frame(const std::string& payload, uint64_t recv_ns);
    void fail_pending_posts(const std::string& reason);

    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    const std::string host_;
    const std::string port_;
    const std::string path_;
    const std::string wallet_address_;
    const std::vector<std::string> pinned_tls_sha256_;

    std::atomic<uint64_t> next_post_id_{1};
    std::mutex pending_posts_mutex_;
    std::unordered_map<uint64_t, std::promise<std::string>> pending_posts_;

    UserFillsHandler user_fills_handler_;
};

}  // namespace bpt::order_gateway::adapter::hyperliquid
