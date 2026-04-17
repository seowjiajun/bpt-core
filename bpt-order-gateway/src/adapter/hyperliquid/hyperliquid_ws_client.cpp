#include "order_gateway/adapter/hyperliquid/hyperliquid_ws_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <yggdrasil/logging.h>
#include <yggdrasil/util/tsc_clock.h>

namespace bpt::order_gateway::adapter::hyperliquid {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

HyperliquidWsClient::HyperliquidWsClient(net::io_context& ioc,
                                         ssl::context& ssl_ctx,
                                         std::string host,
                                         std::string port,
                                         std::string path,
                                         std::string wallet_address)
    : ioc_(ioc),
      ssl_ctx_(ssl_ctx),
      host_(std::move(host)),
      port_(std::move(port)),
      path_(std::move(path)),
      wallet_address_(std::move(wallet_address)) {}

void HyperliquidWsClient::set_user_fills_handler(UserFillsHandler h) {
    user_fills_handler_ = std::move(h);
}

void HyperliquidWsClient::handle_frame(const std::string& payload, uint64_t recv_ns) {
    // Cheap early-outs for small frames HL sends (pong, subscription acks,
    // etc.) — skip the full JSON parse.
    if (payload.size() < 16) return;

    json::value root;
    try {
        root = json::parse(payload);
    } catch (const std::exception&) {
        return;
    }
    if (!root.is_object()) return;
    const auto& obj = root.as_object();

    auto channel_it = obj.find("channel");
    auto data_it = obj.find("data");
    if (channel_it == obj.end() || data_it == obj.end()) return;
    if (!channel_it->value().is_string()) return;

    const std::string_view channel(channel_it->value().as_string());

    if (channel == "userFills") {
        // HL sends { channel:"userFills", data:{ isSnapshot, user, fills:[...] } }.
        // The subscription type is "userFills", and the response channel name
        // matches — do NOT confuse with "user" (a legacy shorthand that HL no
        // longer publishes). A mismatch here silently drops every fill.
        const auto& data = data_it->value().as_object();
        auto fills_it = data.find("fills");
        if (fills_it == data.end()) return;
        if (user_fills_handler_) user_fills_handler_(fills_it->value().as_array(), recv_ns);
        return;
    }

    if (channel == "error") {
        // Protocol-level error (e.g. HL rejecting an envelope it can't
        // parse, like `modify` which isn't supported over WS post).
        // Comes back without an id, so we can't match it to a specific
        // pending post — fail all in-flight senders with the error text
        // so they unblock immediately instead of waiting for the 5 s
        // timeout.
        std::string err;
        if (data_it->value().is_string()) {
            err = std::string(data_it->value().as_string());
        } else {
            err = json::serialize(data_it->value());
        }
        ygg::log::warn("[Heimdall] HyperliquidWsClient: HL WS channel=error: {}",
                       err.substr(0, 200));
        fail_pending_posts("HL WS error: " + err);
        return;
    }

    if (channel == "post") {
        // Post response: {"channel":"post","data":{"id":<N>,"response":{
        //   "type":"action","payload":{...}} | {"type":"error","payload":"msg"}}}
        if (!data_it->value().is_object()) return;
        const auto& data = data_it->value().as_object();

        auto id_it = data.find("id");
        if (id_it == data.end() || !id_it->value().is_int64()) return;
        const uint64_t id = static_cast<uint64_t>(id_it->value().as_int64());

        // Serialize the response.payload (or the error string) — caller
        // parses exactly what the REST /exchange body used to return.
        std::string body;
        auto response_it = data.find("response");
        if (response_it != data.end() && response_it->value().is_object()) {
            const auto& resp = response_it->value().as_object();
            auto type_it = resp.find("type");
            auto payload_it = resp.find("payload");
            if (type_it != resp.end() && payload_it != resp.end() &&
                type_it->value().is_string()) {
                const std::string_view t(type_it->value().as_string());
                if (t == "action") {
                    body = json::serialize(payload_it->value());
                } else if (t == "error") {
                    // Wrap HL error strings in the same shape send_new_order
                    // already handles: {"status":"err","response":"<msg>"}
                    json::object wrapper;
                    wrapper["status"] = "err";
                    wrapper["response"] = payload_it->value();
                    body = json::serialize(wrapper);
                }
            }
        }

        std::lock_guard<std::mutex> lock(pending_posts_mutex_);
        auto it = pending_posts_.find(id);
        if (it == pending_posts_.end()) return;  // stale response
        try {
            it->second.set_value(std::move(body));
        } catch (const std::future_error&) {
            // Promise already satisfied — ignore.
        }
        pending_posts_.erase(it);
    }
}

std::string HyperliquidWsClient::post_action(const json::value& action,
                                              uint64_t nonce,
                                              const SignedTransaction& sig) {
    std::shared_ptr<WsStream> stream;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        stream = stream_;
    }
    if (!stream) {
        throw std::runtime_error("HL WS not connected");
    }

    const uint64_t id = next_post_id_.fetch_add(1, std::memory_order_relaxed);

    json::object signature;
    signature["r"] = "0x" + sig.r;
    signature["s"] = "0x" + sig.s;
    signature["v"] = sig.v;

    json::object payload;
    payload["action"] = action;
    payload["nonce"] = nonce;
    payload["signature"] = std::move(signature);

    json::object inner_request;
    inner_request["type"] = "action";
    inner_request["payload"] = std::move(payload);

    json::object envelope;
    envelope["method"] = "post";
    envelope["id"] = id;
    envelope["request"] = std::move(inner_request);

    const std::string frame = json::serialize(envelope);

    std::future<std::string> fut;
    {
        std::lock_guard<std::mutex> lock(pending_posts_mutex_);
        fut = pending_posts_[id].get_future();
    }

    try {
        std::lock_guard<std::mutex> lock(write_mutex_);
        stream->write(net::buffer(frame));
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(pending_posts_mutex_);
        pending_posts_.erase(id);
        throw;
    }

    // HL p99 is ~2 s, so 5 s is a generous ceiling; anything longer
    // means the connection is likely dead and we surface that as an
    // error so the caller can REJECT and the strategy moves on.
    const auto status = fut.wait_for(std::chrono::seconds(5));
    if (status != std::future_status::ready) {
        std::lock_guard<std::mutex> lock(pending_posts_mutex_);
        pending_posts_.erase(id);
        throw std::runtime_error("HL WS post timeout");
    }

    return fut.get();
}

void HyperliquidWsClient::fail_pending_posts(const std::string& reason) {
    std::lock_guard<std::mutex> lock(pending_posts_mutex_);
    for (auto& [id, promise] : pending_posts_) {
        try {
            promise.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
        } catch (const std::future_error&) {
            // Already satisfied — ignore.
        }
    }
    pending_posts_.clear();
}

void HyperliquidWsClient::run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected) {
    ygg::log::info("[Heimdall] HyperliquidWsClient connecting WS {}:{}{}",
                   host_, port_, path_);

    tcp::resolver resolver(ioc_);
    auto ws = std::make_shared<WsStream>(ioc_, ssl_ctx_);

    auto results = resolver.resolve(host_, port_);
    beast::get_lowest_layer(*ws).connect(results);

    if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), host_.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    ws->next_layer().handshake(ssl::stream_base::client);

    ws->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(beast::http::field::user_agent, "bpt-order-gateway/0.1");
    }));
    ws->handshake(host_, path_);

    // Publish the stream so post_action can write to it from the
    // OrderProcessor thread. Do this only AFTER the handshake completes
    // so senders never see a half-open stream.
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        stream_ = ws;
    }

    // Subscribe to userFills. A placeholder zero address would cause HL
    // to silently reject the subscription, leaving the connection idle
    // and closed after ~60 s.
    if (wallet_address_.empty()) {
        ygg::log::warn("[Heimdall] HyperliquidWsClient: wallet_address empty — "
                       "skipping userFills subscribe. WS will idle-close.");
    } else {
        json::object sub_msg;
        sub_msg["method"] = "subscribe";
        json::object sub_detail;
        sub_detail["type"] = "userFills";
        sub_detail["user"] = wallet_address_;
        sub_msg["subscription"] = sub_detail;
        std::lock_guard<std::mutex> lock(write_mutex_);
        ws->write(net::buffer(json::serialize(sub_msg)));
        ygg::log::info("[Heimdall] HyperliquidWsClient: subscribed userFills for {}", wallet_address_);
    }

    connected.store(true, std::memory_order_relaxed);
    ygg::log::info("[Heimdall] HyperliquidWsClient connected");

    // Dedicated ping thread. HL closes idle WS after ~60 s; Beast's
    // websocket::stream supports concurrent read+write across threads
    // as long as each direction is single-threaded, so the reader stays
    // in the run() loop and the ping thread writes under write_mutex_.
    std::atomic<bool> ping_stop{false};
    std::thread ping_thread([&, ws]() {
        while (!ping_stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 20 && !ping_stop.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (ping_stop.load(std::memory_order_relaxed)) break;
            try {
                static const std::string msg = R"({"method":"ping"})";
                std::lock_guard<std::mutex> lock(write_mutex_);
                ws->write(net::buffer(msg));
                ygg::log::info("[Heimdall] HyperliquidWsClient: ping sent");
            } catch (const std::exception& e) {
                ygg::log::warn("[Heimdall] HyperliquidWsClient: ping write failed: {}", e.what());
                // Don't throw — let the reader detect the dead connection
                // and trigger reconnect via the normal error path.
                break;
            }
        }
    });

    struct JoinGuard {
        std::atomic<bool>& stop;
        std::thread& th;
        ~JoinGuard() {
            stop.store(true, std::memory_order_relaxed);
            if (th.joinable()) th.join();
        }
    } join_guard{ping_stop, ping_thread};

    // On exit (normal or via exception), clear the published stream_
    // and fail any pending post futures so senders never wait forever.
    struct StreamGuard {
        HyperliquidWsClient* self;
        ~StreamGuard() {
            {
                std::lock_guard<std::mutex> lock(self->lifecycle_mutex_);
                self->stream_.reset();
            }
            self->fail_pending_posts("HL WS disconnected");
        }
    } stream_guard{this};

    beast::flat_buffer buf;
    while (!stop_flag.load(std::memory_order_relaxed)) {
        beast::error_code ec;
        ws->read(buf, ec);

        if (ec == beast::error::timeout) {
            buf.consume(buf.size());
            continue;
        }
        if (ec) throw beast::system_error(ec);

        const uint64_t recv_ns = ygg::util::WallClock::now_ns();
        handle_frame(std::string(static_cast<const char*>(buf.data().data()), buf.data().size()),
                     recv_ns);
        buf.consume(buf.size());
    }

    ws->close(websocket::close_code::normal);
}

}  // namespace bpt::order_gateway::adapter::hyperliquid
