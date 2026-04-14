#pragma once

#include "heimdall/adapter/common/credentials.h"
#include "heimdall/adapter/common/order_adapter_base.h"
#include "heimdall/adapter/hyperliquid/hyperliquid_exec_emitter.h"
#include "heimdall/adapter/hyperliquid/hyperliquid_exec_parser.h"
#include "heimdall/adapter/hyperliquid/hyperliquid_signer.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json/fwd.hpp>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace heimdall::adapter {

// HyperliquidOrderAdapter sends orders to the Hyperliquid L1 via signed REST
// calls. Fill events are received from the Hyperliquid WebSocket
// (wss://api.hyperliquid.xyz/ws).
//
// Private key is passed via ExchangeCredentials.private_key (64-char hex).
// If the key is empty, the adapter starts in disabled mode — connect_and_run()
// spins on stop_flag_ rather than attempting a connection.
class HyperliquidOrderAdapter : public OrderAdapterBase {
public:
    HyperliquidOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds);

    void send_new_order(const bifrost::protocol::NewOrder& order) override;
    void send_cancel(const bifrost::protocol::CancelOrder& cancel, const std::string& native_symbol) override;
    void send_cancel_all(uint64_t instrument_id) override;
    void send_modify(const bifrost::protocol::ModifyOrder& modify, const std::string& native_symbol) override;

    [[nodiscard]] bifrost::protocol::ExchangeId::Value exchange_id() const override {
        return bifrost::protocol::ExchangeId::HYPERLIQUID;
    }
    [[nodiscard]] const char* exchange_name() const override { return "HYPERLIQUID"; }
    [[nodiscard]] bool is_connected() const override { return enabled_ && connected_.load(std::memory_order_relaxed); }

    AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) override;

protected:
    void connect_and_run() override;

private:
    void handle_message(const std::string& payload, uint64_t recv_ns);

    // HTTPS POST to Hyperliquid REST endpoint. Still used for /info
    // queries (clearinghouseState, meta) which are unsigned public reads
    // and don't benefit from the WS post path.
    std::string https_post(const std::string& path, const std::string& body);
    void https_connect();   // must be called with https_mutex_ held
    void https_close() noexcept;

    // WebSocket post action API — the fast path for signed exchange actions.
    //
    // HL exposes a `{"method":"post","id":N,"request":{"type":"action",
    // "payload":<signed-action>}}` envelope on the same wss://.../ws
    // connection that we already use for userFills. Responses come back on
    // `channel:"post"` with the matching id, allowing multiple orders to
    // be in-flight at once without the HTTP framing / dispatch cost that
    // /exchange carries per request.
    //
    // ws_post_action blocks the caller until the matching post response
    // arrives or the 5 s timeout fires. Returns the `response.payload`
    // JSON string (same shape the /exchange REST body used to return).
    // Throws on timeout, connection failure, or WS write error.
    std::string ws_post_action(const boost::json::value& action,
                               uint64_t nonce,
                               const SignedTransaction& sig);
    // Fail any pending ws_post_action futures with an error. Called on
    // WS disconnect or shutdown so senders never hang forever.
    void fail_pending_posts(const std::string& reason);

    bool enabled_{false};  // false if private_key credential is empty
    std::string wallet_address_;
    std::unique_ptr<HyperliquidSigner> signer_;
    HyperliquidExecParser parser_;
    hyperliquid::HyperliquidExecEmitter exec_emitter_{exec_queue_};

    // client_order_id → HL exchange oid (from the "resting" response).
    // HL's cancel-by-oid wants the EXCHANGE oid, not our client id, so
    // send_cancel looks up the mapping here. Single-threaded access from
    // the OrderProcessor thread (send_new_order/send_cancel never overlap).
    std::unordered_map<uint64_t, uint64_t> client_to_exch_oid_;

    // Persistent TLS connection to the HL REST endpoint (used by /info).
    std::mutex https_mutex_;
    boost::asio::io_context https_ioc_;
    boost::asio::ssl::context https_ssl_ctx_{boost::asio::ssl::context::tls_client};
    std::unique_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>> https_stream_;

    // WS stream shared with the read loop + ping thread + order senders.
    // shared_ptr so a reconnect in connect_and_run() doesn't pull the
    // rug out from under a concurrent sender mid-write — the old stream
    // stays alive for the duration of any in-progress writes.
    //
    // ws_lifecycle_mutex_ protects the pointer. ws_write_mutex_ serializes
    // concurrent writes (ping, orders, cancels). Reads are single-threaded
    // in connect_and_run()'s loop and don't contend with writes (Beast's
    // websocket::stream supports concurrent read+write across threads as
    // long as each direction is single-threaded).
    using WsStream = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;
    std::mutex ws_lifecycle_mutex_;
    std::mutex ws_write_mutex_;
    std::shared_ptr<WsStream> ws_stream_;

    // Pending post-action requests, keyed by the monotonic `id` we send.
    // When a `channel:"post"` frame arrives the reader loop looks up the
    // matching promise and sets its value with the response payload.
    std::atomic<uint64_t> next_post_id_{1};
    std::mutex pending_posts_mutex_;
    std::unordered_map<uint64_t, std::promise<std::string>> pending_posts_;
};

}  // namespace heimdall::adapter
