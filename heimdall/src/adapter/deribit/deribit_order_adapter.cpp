#include "heimdall/adapter/deribit/deribit_order_adapter.h"

#include <bifrost_protocol/ExchangeId.h>
#include <bifrost_protocol/ExecStatus.h>
#include <bifrost_protocol/FeeCurrency.h>
#include <bifrost_protocol/OrderSide.h>
#include <bifrost_protocol/OrderType.h>
#include <bifrost_protocol/RejectReason.h>
#include <spdlog/spdlog.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <stdexcept>
#include <string>

#include "heimdall/adapter/common/credentials.h"

namespace heimdall::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

static constexpr double kScale = 1e8;

DeribitOrderAdapter::DeribitOrderAdapter(const config::AdapterConfig& cfg,
                                         const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg), client_id_(creds.client_id), client_secret_(creds.client_secret) {
    uint32_t epoch_s =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", epoch_s);
    session_prefix_ = std::string(buf, 8);

    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            spdlog::error("[Deribit] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };
}

std::string DeribitOrderAdapter::build_auth_msg() {
    json::object msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = jsonrpc_id_.fetch_add(1, std::memory_order_relaxed);
    msg["method"] = "public/auth";
    json::object params;
    params["grant_type"] = "client_credentials";
    params["client_id"] = client_id_;
    params["client_secret"] = client_secret_;
    msg["params"] = std::move(params);
    return json::serialize(msg);
}

void DeribitOrderAdapter::handle_message(const std::string& payload, uint64_t recv_ns) {
    spdlog::info("[Heimdall] DeribitOrderAdapter WS rx: {}", payload.substr(0, 500));
    auto root = json::parse(payload);
    if (!root.is_object()) return;
    const auto& obj = root.as_object();

    // Notification methods: heartbeat + subscription channel pushes
    if (auto method_it = obj.find("method"); method_it != obj.end()) {
        std::string method = std::string(method_it->value().as_string());

        if (method == "heartbeat") {
            auto params_it = obj.find("params");
            if (params_it != obj.end() && params_it->value().is_object()) {
                auto type_it = params_it->value().as_object().find("type");
                if (type_it != params_it->value().as_object().end() &&
                    std::string(type_it->value().as_string()) == "test_request") {
                    json::object resp;
                    resp["jsonrpc"] = "2.0";
                    resp["id"] = jsonrpc_id_.fetch_add(1, std::memory_order_relaxed);
                    resp["method"] = "public/test";
                    resp["params"] = json::object{};
                    std::lock_guard<std::mutex> lk(send_mu_);
                    if (ws_send_) ws_send_(json::serialize(resp));
                }
            }
            return;
        }

        if (method == "subscription") {
            auto params_it = obj.find("params");
            if (params_it == obj.end() || !params_it->value().is_object()) return;
            const auto& params = params_it->value().as_object();
            auto channel_it = params.find("channel");
            if (channel_it == params.end()) return;
            if (std::string(channel_it->value().as_string()) == "user.orders.any.raw") {
                auto data_it = params.find("data");
                if (data_it != params.end() && data_it->value().is_object())
                    parser_.handle_subscription_event(data_it->value().as_object(), recv_ns);
            }
        }
        return;
    }

    // JSON-RPC responses (id-based)
    if (obj.find("id") == obj.end()) return;

    if (auto err_it = obj.find("error"); err_it != obj.end()) {
        const auto& err = err_it->value().as_object();
        int64_t code = 0;
        std::string errmsg;
        if (auto cit = err.find("code"); cit != err.end()) code = cit->value().to_number<int64_t>();
        if (auto mit = err.find("message"); mit != err.end())
            errmsg = std::string(mit->value().as_string());
        spdlog::error("[Heimdall] DeribitOrderAdapter: JSON-RPC error code={} msg={}", code,
                      errmsg);
        return;
    }

    auto result_it = obj.find("result");
    if (result_it == obj.end() || !result_it->value().is_object()) return;
    const auto& res = result_it->value().as_object();

    // Auth response
    if (res.find("access_token") != res.end()) {
        spdlog::info("[Heimdall] DeribitOrderAdapter: authenticated successfully");
        logged_in_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(send_mu_);
        auto send_rpc = [&](json::object msg) {
            msg["jsonrpc"] = "2.0";
            msg["id"] = jsonrpc_id_.fetch_add(1, std::memory_order_relaxed);
            if (ws_send_) ws_send_(json::serialize(msg));
        };
        send_rpc({{"method", "private/enable_cancel_on_disconnect"}, {"params", json::object{}}});
        send_rpc({{"method", "public/set_heartbeat"}, {"params", json::object{{"interval", 10}}}});
        send_rpc({{"method", "private/subscribe"},
                  {"params", json::object{{"channels", json::array{"user.orders.any.raw"}}}}});
        for (const auto& pending : pending_sends_)
            if (ws_send_) ws_send_(pending);
        pending_sends_.clear();
        return;
    }

    // Order response (private/buy or private/sell)
    if (auto order_it = res.find("order"); order_it != res.end())
        parser_.handle_order_response(order_it->value().as_object(), recv_ns);
}

void DeribitOrderAdapter::connect_and_run() {
    logged_in_.store(false, std::memory_order_relaxed);
    parser_.reset();

    spdlog::info("[Heimdall] DeribitOrderAdapter connecting {}:{}{}", cfg_.ws_host, cfg_.ws_port,
                 cfg_.ws_path);

    tcp::resolver resolver(ioc_);
    auto ws =
        std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(ioc_, ssl_ctx_);

    auto results = resolver.resolve(cfg_.ws_host, cfg_.ws_port);
    beast::get_lowest_layer(*ws).connect(results);

    if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), cfg_.ws_host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    ws->next_layer().handshake(ssl::stream_base::client);

    // Deribit uses text frames for JSON-RPC
    ws->text(true);

    ws->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "heimdall/0.1");
    }));
    ws->handshake(cfg_.ws_host, cfg_.ws_path);

    // Disable Beast's idle timeout — we manage heartbeats via Deribit's
    // set_heartbeat + test_request protocol.
    ws->set_option(websocket::stream_base::timeout{
        std::chrono::seconds(15),        // handshake timeout
        websocket::stream_base::none(),  // no idle timeout
        false                            // no Beast keep-alive pings
    });

    // Register send callback — nulled out on any exit path.
    {
        std::lock_guard<std::mutex> lk(send_mu_);
        ws_send_ = [&ws](const std::string& msg) { ws->write(net::buffer(msg)); };
    }

    ws->write(net::buffer(build_auth_msg()));

    connected_.store(true, std::memory_order_relaxed);
    spdlog::info("[Heimdall] DeribitOrderAdapter connected, waiting for auth");

    try {
        beast::flat_buffer buf;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(30));
            beast::error_code ec;
            ws->read(buf, ec);

            if (ec == beast::error::timeout) {
                buf.consume(buf.size());
                continue;
            }
            if (ec) throw beast::system_error(ec);

            uint64_t recv_ns =
                static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
            std::string msg(static_cast<const char*>(buf.data().data()), buf.data().size());
            buf.consume(buf.size());

            handle_message(msg, recv_ns);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lk(send_mu_);
        ws_send_ = nullptr;
        throw;
    }

    {
        std::lock_guard<std::mutex> lk(send_mu_);
        ws_send_ = nullptr;
    }
    ws->close(websocket::close_code::normal);
}

void DeribitOrderAdapter::send_new_order(const bifrost::protocol::NewOrder& order) {
    using OT = bifrost::protocol::OrderType;
    using OS = bifrost::protocol::OrderSide;
    using TIF = bifrost::protocol::TimeInForce;

    const std::string exchange_symbol = order.getExchangeSymbolAsString();

    std::string label = session_prefix_ + "G" + std::to_string(order.orderId());
    parser_.register_order(label, order.orderId());

    // Determine method: private/buy or private/sell
    std::string method = (order.side() == OS::BUY) ? "private/buy" : "private/sell";

    // Order type
    std::string type_str;
    if (order.orderType() == OT::MARKET)
        type_str = "market";
    else
        type_str = "limit";

    // Time in force
    std::string tif_str;
    switch (order.timeInForce()) {
        case TIF::IOC:
            tif_str = "immediate_or_cancel";
            break;
        case TIF::FOK:
            tif_str = "fill_or_kill";
            break;
        default:
            tif_str = "good_til_cancelled";
            break;
    }

    // Convert price and quantity from 1e8 fixed-point to decimal
    double price = static_cast<double>(order.price()) / kScale;
    double amount = static_cast<double>(order.quantity()) / kScale;

    json::object params;
    params["instrument_name"] = exchange_symbol;
    params["amount"] = amount;
    params["type"] = type_str;
    params["label"] = label;
    if (order.orderType() != OT::MARKET) {
        params["price"] = price;
        params["time_in_force"] = tif_str;
    }

    json::object msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = jsonrpc_id_.fetch_add(1, std::memory_order_relaxed);
    msg["method"] = method;
    msg["params"] = std::move(params);

    std::string frame = json::serialize(msg);

    auto emit_rejection = [&]() {
        uint64_t ts =
            static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        ExecEvent rej{};
        rej.order_id = order.orderId();
        rej.exchange_id = bifrost::protocol::ExchangeId::DERIBIT;
        rej.instrument_id = 0;
        rej.local_ts_ns = ts;
        rej.exchange_ts_ns = ts;
        rej.side = (order.side() == OS::BUY) ? OS::BUY : OS::SELL;
        rej.order_type = order.orderType();
        rej.status = bifrost::protocol::ExecStatus::REJECTED;
        rej.reject_reason = bifrost::protocol::RejectReason::EXCHANGE_ERROR;
        if (!exec_queue_.try_push(rej))
            spdlog::error("[Deribit] exec_queue full — dropped rejection order_id={}", rej.order_id);
    };

    std::lock_guard<std::mutex> lk(send_mu_);
    if (!ws_send_) {
        spdlog::warn(
            "[Heimdall] DeribitOrderAdapter: send_new_order: WS not "
            "connected, rejecting order={}",
            order.orderId());
        emit_rejection();
        return;
    }
    if (!logged_in_.load(std::memory_order_acquire)) {
        spdlog::info(
            "[Heimdall] DeribitOrderAdapter: queuing order {} (not yet "
            "authenticated)",
            order.orderId());
        pending_sends_.push_back(frame);
        return;
    }
    try {
        ws_send_(frame);
    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] DeribitOrderAdapter: send_new_order failed: {}", e.what());
        emit_rejection();
    }
}

void DeribitOrderAdapter::send_cancel(const bifrost::protocol::CancelOrder& cancel,
                                      const std::string& /*native_symbol*/) {
    // Deribit cancel uses exchange order_id, not instrument symbol.
    std::string exch_oid = parser_.get_exchange_order_id(cancel.orderId());
    if (exch_oid.empty()) {
        spdlog::warn(
            "[Heimdall] DeribitOrderAdapter: send_cancel: no exchange "
            "order_id for order={}",
            cancel.orderId());
        return;
    }

    json::object params;
    params["order_id"] = exch_oid;

    json::object msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = jsonrpc_id_.fetch_add(1, std::memory_order_relaxed);
    msg["method"] = "private/cancel";
    msg["params"] = std::move(params);

    std::string frame = json::serialize(msg);
    std::lock_guard<std::mutex> lk(send_mu_);
    if (ws_send_) {
        try {
            ws_send_(frame);
        } catch (const std::exception& e) {
            spdlog::error("[Heimdall] DeribitOrderAdapter: send_cancel failed: {}", e.what());
        }
    }
}

void DeribitOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    // Deribit cancel_all_by_instrument requires instrument_name, but we only
    // have instrument_id here. Log a warning — in practice the main.cpp
    // cancel_all path iterates open orders and calls send_cancel per order.
    spdlog::warn(
        "[Heimdall] DeribitOrderAdapter: send_cancel_all called "
        "instrument_id={} — not supported without instrument name",
        instrument_id);
}

void DeribitOrderAdapter::send_modify(const bifrost::protocol::ModifyOrder& modify,
                                      const std::string& /*native_symbol*/) {
    std::string exch_oid = parser_.get_exchange_order_id(modify.orderId());
    if (exch_oid.empty()) {
        spdlog::warn(
            "[Heimdall] DeribitOrderAdapter: send_modify: no exchange "
            "order_id for order={}",
            modify.orderId());
        return;
    }

    double new_price = static_cast<double>(modify.newPrice()) / kScale;
    double new_amount = static_cast<double>(modify.newQuantity()) / kScale;

    json::object params;
    params["order_id"] = exch_oid;
    params["amount"] = new_amount;
    params["price"] = new_price;

    json::object msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = jsonrpc_id_.fetch_add(1, std::memory_order_relaxed);
    msg["method"] = "private/edit";
    msg["params"] = std::move(params);

    std::string frame = json::serialize(msg);
    std::lock_guard<std::mutex> lk(send_mu_);
    if (ws_send_) {
        try {
            ws_send_(frame);
        } catch (const std::exception& e) {
            spdlog::error("[Heimdall] DeribitOrderAdapter: send_modify failed: {}", e.what());
        }
    }
}

AccountSnapshotData DeribitOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    // Not yet implemented — Deribit account snapshot is not required for current strategies.
    spdlog::warn(
        "[Heimdall] DeribitOrderAdapter: fetch_account_snapshot not implemented — returning empty "
        "snapshot");
    AccountSnapshotData snap;
    snap.exchange_id = bifrost::protocol::ExchangeId::DERIBIT;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
    return snap;
}

}  // namespace heimdall::adapter
