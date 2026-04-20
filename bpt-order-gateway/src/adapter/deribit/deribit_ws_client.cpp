#include "order_gateway/adapter/deribit/deribit_ws_client.h"

#include <boost/beast/websocket.hpp>
#include <chrono>
#include <bpt_common/logging.h>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::order_gateway::adapter::deribit {

namespace websocket = boost::beast::websocket;

DeribitWsClient::DeribitWsClient(boost::asio::io_context& ioc,
                                  boost::asio::ssl::context& ssl_ctx,
                                  const config::AdapterConfig& cfg)
    : ioc_(ioc), ssl_ctx_(ssl_ctx), cfg_(cfg) {}

void DeribitWsClient::set_message_handler(MessageHandler h) {
    message_handler_ = std::move(h);
}

void DeribitWsClient::set_login_msg_builder(LoginMsgBuilder b) {
    login_msg_builder_ = std::move(b);
}

void DeribitWsClient::on_handshake_complete() {
    if (login_msg_builder_) send(login_msg_builder_());
}

void DeribitWsClient::on_frame(std::string_view payload, uint64_t recv_ns) {
    if (message_handler_) message_handler_(std::string(payload), recv_ns);
}

void DeribitWsClient::run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected) {
    bpt::common::log::info("DeribitWsClient connecting {}:{}{}",
                   cfg_.ws_host, cfg_.ws_port, cfg_.ws_path);

    auto ws_ptr = bpt::common::ws::ws_connect(ioc_, ssl_ctx_, cfg_.ws_host, cfg_.ws_port, cfg_.ws_path,
                                      /*so_rcvbuf_bytes=*/0,
                                      /*connect_timeout_ms=*/30000,
                                      /*user_agent=*/"bpt-order-gateway/0.1",
                                      cfg_.pinned_tls_sha256);

    // Disable Beast's idle timeout — heartbeats are driven by Deribit's
    // application-level set_heartbeat + test_request protocol handled
    // inside on_frame.
    ws_ptr->set_option(websocket::stream_base::timeout{
        std::chrono::seconds(15),        // handshake timeout (unused post-connect)
        websocket::stream_base::none(),  // no idle timeout
        false,                           // no Beast keep-alive pings
    });

    bpt::common::log::info("DeribitWsClient connected, waiting for auth");
    RunLoop::run(bpt::common::ws::AnyWsStream(std::move(ws_ptr)), stop_flag, connected);
}

}  // namespace bpt::order_gateway::adapter::deribit
