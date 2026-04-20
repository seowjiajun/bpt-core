#include "order_gateway/adapter/okx/okx_ws_client.h"

#include <boost/beast/websocket.hpp>
#include <chrono>
#include <bpt_common/logging.h>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::order_gateway::adapter::okx {

namespace websocket = boost::beast::websocket;

OKXWsClient::OKXWsClient(boost::asio::io_context& ioc,
                         boost::asio::ssl::context& ssl_ctx,
                         const config::AdapterConfig& cfg)
    : ioc_(ioc), ssl_ctx_(ssl_ctx), cfg_(cfg) {}

void OKXWsClient::set_message_handler(MessageHandler h) {
    message_handler_ = std::move(h);
}

void OKXWsClient::set_login_msg_builder(LoginMsgBuilder b) {
    login_msg_builder_ = std::move(b);
}

void OKXWsClient::on_handshake_complete() {
    if (login_msg_builder_) send(login_msg_builder_());
}

void OKXWsClient::on_frame(std::string_view payload, uint64_t recv_ns) {
    // OKX sends a raw "ping" text frame as its heartbeat probe; reply
    // with "pong". Also swallow our own "pong" if the server reflects it
    // back. Neither needs to reach the adapter's message handler.
    if (payload == "ping") { send("pong"); return; }
    if (payload == "pong") return;
    if (message_handler_) message_handler_(std::string(payload), recv_ns);
}

std::optional<bpt::common::ws::PingConfig> OKXWsClient::ping_config() const {
    // OKX closes idle connections at 30s; 10s ping keeps us well
    // inside that window with a margin for network jitter.
    return bpt::common::ws::PingConfig{
        std::chrono::seconds(10),
        [] {
            bpt::common::log::info("[OrderGateway] OKXWsClient heartbeat ping sent");
            return std::string("ping");
        },
    };
}

void OKXWsClient::run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected) {
    bpt::common::log::info("[OrderGateway] OKXWsClient connecting {}:{}{} (tls={})",
                   cfg_.ws_host, cfg_.ws_port, cfg_.ws_path, cfg_.use_tls);

    if (!cfg_.use_tls) {
        // Plain TCP — used when connecting to a local simulation server
        // for backtests. IMPORTANT: do NOT set stream_base::timeout
        // here; installing it would take over the lowest-layer timer
        // and silently nullify the read-timeout in RunLoop, breaking
        // the periodic wakeup that lets the ping thread actually fire.
        auto ws_ptr = bpt::common::ws::ws_connect_plain(ioc_, cfg_.ws_host, cfg_.ws_port, cfg_.ws_path,
                                                /*so_rcvbuf_bytes=*/0,
                                                /*connect_timeout_ms=*/30000,
                                                /*user_agent=*/"bpt-order-gateway/0.1");
        bpt::common::log::info("[OrderGateway] OKXWsClient connected (plain), waiting for login");
        RunLoop::run(bpt::common::ws::AnyWsStream(std::move(ws_ptr)), stop_flag, connected);
        return;
    }

    // TLS path — production.
    auto ws_ptr = bpt::common::ws::ws_connect(ioc_, ssl_ctx_, cfg_.ws_host, cfg_.ws_port, cfg_.ws_path,
                                      /*so_rcvbuf_bytes=*/0,
                                      /*connect_timeout_ms=*/30000,
                                      /*user_agent=*/"bpt-order-gateway/0.1",
                                      cfg_.pinned_tls_sha256);
    // Handshake-phase timeout (15s) is unused now that connection is
    // already established, but setting idle=none here would clobber
    // RunLoop's per-read timeout. Leave stream_base::timeout unset on
    // the TLS path too — RunLoop drives both timing and heartbeat.
    bpt::common::log::info("[OrderGateway] OKXWsClient connected (tls), waiting for login");
    RunLoop::run(bpt::common::ws::AnyWsStream(std::move(ws_ptr)), stop_flag, connected);
}

}  // namespace bpt::order_gateway::adapter::okx
