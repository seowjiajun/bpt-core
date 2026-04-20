#include "order_gateway/adapter/binance/binance_user_data_ws.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::order_gateway::adapter::binance {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

BinanceUserDataWs::BinanceUserDataWs(net::io_context& ioc,
                                      ssl::context& ssl_ctx,
                                      const config::AdapterConfig& cfg,
                                      BinanceHttpsClient& https)
    : ioc_(ioc), ssl_ctx_(ssl_ctx), cfg_(cfg), https_(https) {}

void BinanceUserDataWs::set_message_handler(MessageHandler h) {
    message_handler_ = std::move(h);
}

std::string BinanceUserDataWs::create_listen_key() {
    std::string resp = https_.request("POST", "/api/v3/userDataStream", "", true);
    try {
        auto root = json::parse(resp);
        if (!root.is_object())
            return "";
        if (root.as_object().contains("listenKey"))
            return std::string(root.as_object().at("listenKey").as_string());
    } catch (const std::exception&) {
        // Endpoint gone (e.g. testnet 410) — REST-only mode
    }
    return "";
}

void BinanceUserDataWs::extend_listen_key(const std::string& listen_key) {
    https_.request("PUT", "/api/v3/userDataStream?listenKey=" + listen_key, "", true);
}

void BinanceUserDataWs::delete_listen_key(const std::string& listen_key) {
    https_.request("DELETE", "/api/v3/userDataStream?listenKey=" + listen_key, "", true);
}

void BinanceUserDataWs::run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected) {
    bpt::common::log::info("BinanceUserDataWs: creating listen key");
    const std::string listen_key = create_listen_key();

    if (listen_key.empty()) {
        bpt::common::log::warn(
            "BinanceUserDataWs: no listen key — REST-only "
            "mode (exec reports from order response)");
        connected.store(true, std::memory_order_relaxed);
        while (!stop_flag.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::seconds(1));
        return;
    }

    bpt::common::log::info("BinanceUserDataWs: connecting WS for user data stream");
    const std::string ws_path = cfg_.ws_path + "/" + listen_key;

    tcp::resolver resolver(ioc_);
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc_, ssl_ctx_);

    auto results = resolver.resolve(cfg_.ws_host, cfg_.ws_port);
    beast::get_lowest_layer(ws).connect(results);

    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), cfg_.ws_host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    ws.next_layer().handshake(ssl::stream_base::client);

    ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) { req.set(beast::http::field::user_agent, "bpt-order-gateway/0.1"); }));
    ws.handshake(cfg_.ws_host, ws_path);

    connected.store(true, std::memory_order_relaxed);
    bpt::common::log::info("BinanceUserDataWs: connected");

    auto last_ping = std::chrono::steady_clock::now();

    beast::flat_buffer buf;
    while (!stop_flag.load(std::memory_order_relaxed)) {
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));

        beast::error_code ec;
        ws.read(buf, ec);

        if (ec == beast::error::timeout) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_ping >= std::chrono::minutes(30)) {
                extend_listen_key(listen_key);
                last_ping = now;
            }
            buf.consume(buf.size());
            continue;
        }
        if (ec)
            throw beast::system_error(ec);

        const uint64_t recv_ns = bpt::common::util::WallClock::now_ns();
        std::string payload(static_cast<const char*>(buf.data().data()), buf.data().size());
        buf.consume(buf.size());

        if (message_handler_)
            message_handler_(payload, recv_ns);
    }

    try {
        delete_listen_key(listen_key);
    } catch (const std::exception& e) {
        bpt::common::log::warn("BinanceUserDataWs: delete_listen_key failed: {}", e.what());
    }
    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
}

}  // namespace bpt::order_gateway::adapter::binance
