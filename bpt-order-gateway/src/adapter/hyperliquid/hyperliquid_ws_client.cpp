#include "order_gateway/adapter/hyperliquid/hyperliquid_ws_client.h"

#include <boost/beast/core.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <bpt_common/logging.h>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::order_gateway::adapter::hyperliquid {

namespace json = boost::json;

HyperliquidWsClient::HyperliquidWsClient(boost::asio::io_context& ioc,
                                         boost::asio::ssl::context& ssl_ctx,
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

void HyperliquidWsClient::on_handshake_complete() {
    if (wallet_address_.empty()) {
        bpt::common::log::warn("[OrderGateway] HyperliquidWsClient: wallet_address empty — "
                       "skipping userFills subscribe. WS will idle-close.");
        return;
    }
    json::object sub_detail;
    sub_detail["type"] = "userFills";
    sub_detail["user"] = wallet_address_;
    json::object sub_msg;
    sub_msg["method"] = "subscribe";
    sub_msg["subscription"] = std::move(sub_detail);

    // Truncated-address form — see commit 26a04ad's log-leak audit.
    const auto truncate = [](const std::string& a) {
        return a.size() > 10 ? a.substr(0, 6) + "…" + a.substr(a.size() - 4) : std::string{"<short>"};
    };
    if (!send(json::serialize(sub_msg))) {
        bpt::common::log::warn("[OrderGateway] HyperliquidWsClient: userFills subscribe send failed "
                       "(not connected). WS will idle-close.");
        return;
    }
    bpt::common::log::info("[OrderGateway] HyperliquidWsClient: subscribed userFills for {}",
                   truncate(wallet_address_));
}

void HyperliquidWsClient::on_frame(std::string_view payload, uint64_t recv_ns) {
    // Delegate to the legacy handle_frame implementation which takes a
    // std::string. Cheap copy — frames are small, and the JSON parser
    // works from std::string.
    handle_frame(std::string(payload), recv_ns);
}

std::optional<bpt::common::ws::PingConfig> HyperliquidWsClient::ping_config() const {
    // HL closes idle WS at ~60 s; 20 s keeps us well inside the window
    // with margin for network jitter.
    return bpt::common::ws::PingConfig{
        std::chrono::seconds(20),
        [] {
            bpt::common::log::info("[OrderGateway] HyperliquidWsClient: ping sent");
            return std::string{R"({"method":"ping"})"};
        },
    };
}

void HyperliquidWsClient::handle_frame(const std::string& payload, uint64_t /*recv_ns*/) {
    // Cheap early-outs for small frames HL sends (pong, subscription
    // acks, etc.) — skip the full JSON parse.
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
        // Do NOT confuse with "user" (a legacy shorthand that HL no
        // longer publishes). A mismatch here silently drops every fill.
        const auto& data = data_it->value().as_object();
        auto fills_it = data.find("fills");
        if (fills_it == data.end()) return;
        if (user_fills_handler_) user_fills_handler_(fills_it->value().as_array(), /*recv_ns=*/0);
        return;
    }

    if (channel == "error") {
        // Protocol-level error, no id — fail ALL in-flight senders
        // with the error text so they unblock immediately instead of
        // waiting for the 5 s timeout.
        std::string err;
        if (data_it->value().is_string()) {
            err = std::string(data_it->value().as_string());
        } else {
            err = json::serialize(data_it->value());
        }
        bpt::common::log::warn("[OrderGateway] HyperliquidWsClient: HL WS channel=error: {}",
                       err.substr(0, 200));
        fail_pending_posts("HL WS error: " + err);
        return;
    }

    if (channel == "post") {
        if (!data_it->value().is_object()) return;
        const auto& data = data_it->value().as_object();

        auto id_it = data.find("id");
        if (id_it == data.end() || !id_it->value().is_int64()) return;
        const uint64_t id = static_cast<uint64_t>(id_it->value().as_int64());

        // Serialize response.payload (or error string) — caller parses
        // exactly what the REST /exchange body used to return.
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

    // RunLoop::send handles the thread-safe write; returns false if
    // the WS isn't currently connected (before run() starts or after
    // it returns). Throws on raw write error.
    try {
        if (!send(frame)) {
            std::lock_guard<std::mutex> lock(pending_posts_mutex_);
            pending_posts_.erase(id);
            throw std::runtime_error("HL WS not connected");
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(pending_posts_mutex_);
        pending_posts_.erase(id);
        throw;
    }

    // HL p99 is ~2 s, so 5 s is a generous ceiling; anything longer
    // means the connection is likely dead.
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
    bpt::common::log::info("[OrderGateway] HyperliquidWsClient connecting WS {}:{}{}",
                   host_, port_, path_);

    auto ws_ptr = bpt::common::ws::ws_connect(ioc_, ssl_ctx_, host_, port_, path_,
                                      /*so_rcvbuf_bytes=*/0,
                                      /*connect_timeout_ms=*/30000,
                                      /*user_agent=*/"bpt-order-gateway/0.1");

    // Fail any in-flight posts on exit, whether clean or exceptional.
    // RunLoop's own SendGuard clears its internal stream pointer so
    // RunLoop::send() returns false thereafter; new callers get "not
    // connected" cleanly. This guard handles callers that were already
    // parked on their future when the connection dropped.
    struct PendingGuard {
        HyperliquidWsClient* self;
        ~PendingGuard() { self->fail_pending_posts("HL WS disconnected"); }
    } pending_guard{this};

    bpt::common::log::info("[OrderGateway] HyperliquidWsClient connected");
    RunLoop::run(bpt::common::ws::AnyWsStream(std::move(ws_ptr)), stop_flag, connected);
}

}  // namespace bpt::order_gateway::adapter::hyperliquid
