#include "heimdall/adapter/hyperliquid/hyperliquid_order_adapter.h"

#include <bifrost_protocol/ExchangeId.h>
#include <bifrost_protocol/OrderSide.h>
#include <bifrost_protocol/OrderType.h>
#include <bifrost_protocol/RejectReason.h>
#include <spdlog/spdlog.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
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
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

static constexpr double kScale = 1e8;

HyperliquidOrderAdapter::HyperliquidOrderAdapter(const config::AdapterConfig& cfg,
                                                 const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg), wallet_address_(creds.wallet_address) {
    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            spdlog::error("[Hyperliquid] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };

    if (creds.private_key.empty()) {
        enabled_ = false;
        spdlog::warn("[Heimdall] HyperliquidOrderAdapter: disabled — private_key not set");
        return;
    }
    try {
        signer_ = std::make_unique<HyperliquidSigner>(creds.private_key);
        enabled_ = true;
        spdlog::info("[Heimdall] HyperliquidOrderAdapter: signer loaded");
    } catch (const std::exception& e) {
        enabled_ = false;
        spdlog::warn("[Heimdall] HyperliquidOrderAdapter: disabled — {}", e.what());
    }
}

std::string HyperliquidOrderAdapter::https_post(const std::string& path, const std::string& body) {
    net::io_context ioc;
    ssl::context ssl_ctx(ssl::context::tls_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);

    if (!SSL_set_tlsext_host_name(stream.native_handle(), cfg_.rest_host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");

    auto results = resolver.resolve(cfg_.rest_host, cfg_.rest_port);
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req(http::verb::post, path, 11);
    req.set(http::field::host, cfg_.rest_host);
    req.set(http::field::user_agent, "heimdall/0.1");
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();

    http::write(stream, req);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);

    beast::error_code ec;
    stream.shutdown(ec);

    return res.body();
}

void HyperliquidOrderAdapter::handle_message(const std::string& payload, uint64_t recv_ns) {
    auto root = json::parse(payload);
    if (!root.is_object()) return;
    const auto& obj = root.as_object();

    auto channel_it = obj.find("channel");
    auto data_it = obj.find("data");
    if (channel_it == obj.end() || data_it == obj.end()) return;

    if (std::string(channel_it->value().as_string()) == "user") {
        const auto& data = data_it->value().as_object();
        auto fills_it = data.find("fills");
        if (fills_it == data.end()) return;
        parser_.handle_fills(fills_it->value().as_array(), recv_ns);
    }
}

void HyperliquidOrderAdapter::connect_and_run() {
    if (!enabled_) {
        spdlog::warn("[Heimdall] HyperliquidOrderAdapter: running in disabled mode");
        while (!stop_flag_.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::seconds(1));
        return;
    }

    spdlog::info("[Heimdall] HyperliquidOrderAdapter connecting WS {}:{}{}", cfg_.ws_host,
                 cfg_.ws_port, cfg_.ws_path);

    tcp::resolver resolver(ioc_);
    auto ws =
        std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(ioc_, ssl_ctx_);

    auto results = resolver.resolve(cfg_.ws_host, cfg_.ws_port);
    beast::get_lowest_layer(*ws).connect(results);

    if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), cfg_.ws_host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    ws->next_layer().handshake(ssl::stream_base::client);

    ws->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "heimdall/0.1");
    }));
    ws->handshake(cfg_.ws_host, cfg_.ws_path);

    // Subscribe to user fills
    json::object sub_msg;
    sub_msg["method"] = "subscribe";
    json::object sub_detail;
    sub_detail["type"] = "userFills";
    // Note: Hyperliquid requires a user address for userFills subscription.
    // In production, derive from the private key.
    sub_detail["user"] = "0x0000000000000000000000000000000000000000";
    sub_msg["subscription"] = sub_detail;
    ws->write(net::buffer(json::serialize(sub_msg)));

    connected_.store(true, std::memory_order_relaxed);
    spdlog::info("[Heimdall] HyperliquidOrderAdapter connected");

    beast::flat_buffer buf;
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(30));
        beast::error_code ec;
        ws->read(buf, ec);

        if (ec == beast::error::timeout) {
            json::object ping;
            ping["method"] = "ping";
            ws->write(net::buffer(json::serialize(ping)));
            buf.consume(buf.size());
            continue;
        }
        if (ec) throw beast::system_error(ec);

        uint64_t recv_ns =
            static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        handle_message(std::string(static_cast<const char*>(buf.data().data()), buf.data().size()),
                       recv_ns);
        buf.consume(buf.size());
    }

    ws->close(websocket::close_code::normal);
}

void HyperliquidOrderAdapter::send_new_order(const bifrost::protocol::NewOrder& order) {
    if (!enabled_ || !signer_) {
        spdlog::error("[Heimdall] HyperliquidOrderAdapter: disabled, cannot send order");
        return;
    }

    using OS = bifrost::protocol::OrderSide;

    const std::string exchange_symbol = order.getExchangeSymbolAsString();

    OrderSignParams params;
    params.coin = exchange_symbol;
    params.is_buy = (order.side() == OS::BUY);
    params.price = static_cast<double>(order.price()) / kScale;
    params.size = static_cast<double>(order.quantity()) / kScale;
    params.cloid = order.orderId();

    try {
        auto tx = signer_->sign_order(params);

        // Build Hyperliquid order request JSON
        json::object action;
        action["type"] = "order";

        json::object ord;
        json::object asset;
        asset["coin"] = exchange_symbol;
        ord["a"] = asset;
        ord["b"] = params.is_buy;
        ord["p"] = std::to_string(params.price);
        ord["s"] = std::to_string(params.size);
        ord["r"] = params.reduce_only;
        ord["t"] = json::object{{"limit", json::object{{"tif", "Gtc"}}}};
        ord["c"] = std::to_string(order.orderId());

        json::array orders;
        orders.push_back(ord);
        action["orders"] = std::move(orders);
        action["grouping"] = "na";

        json::object signature;
        signature["r"] = "0x" + tx.r;
        signature["s"] = "0x" + tx.s;
        signature["v"] = tx.v;

        json::object req;
        req["action"] = std::move(action);
        req["nonce"] = tx.nonce;
        req["signature"] = std::move(signature);

        std::string resp = https_post("/exchange", json::serialize(req));
        spdlog::debug("[Heimdall] HyperliquidOrderAdapter: new order resp={}", resp);
    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] HyperliquidOrderAdapter: send_new_order failed: {}", e.what());
    }
}

void HyperliquidOrderAdapter::send_cancel(const bifrost::protocol::CancelOrder& cancel,
                                          const std::string& native_symbol) {
    if (!enabled_ || !signer_) {
        spdlog::error("[Heimdall] HyperliquidOrderAdapter: disabled, cannot cancel order");
        return;
    }

    try {
        auto tx = signer_->sign_cancel(native_symbol, cancel.orderId());

        json::object action;
        action["type"] = "cancel";

        json::array cancels;
        json::object c;
        json::object asset;
        asset["coin"] = native_symbol;
        c["a"] = asset;
        c["o"] = cancel.orderId();
        cancels.push_back(c);
        action["cancels"] = std::move(cancels);

        json::object signature;
        signature["r"] = "0x" + tx.r;
        signature["s"] = "0x" + tx.s;
        signature["v"] = tx.v;

        json::object req;
        req["action"] = std::move(action);
        req["nonce"] = tx.nonce;
        req["signature"] = std::move(signature);

        std::string resp = https_post("/exchange", json::serialize(req));
        spdlog::debug("[Heimdall] HyperliquidOrderAdapter: cancel resp={}", resp);
    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] HyperliquidOrderAdapter: send_cancel failed: {}", e.what());
    }
}

void HyperliquidOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    spdlog::warn("[Heimdall] HyperliquidOrderAdapter: send_cancel_all instrument_id={}",
                 instrument_id);
}

void HyperliquidOrderAdapter::send_modify(const bifrost::protocol::ModifyOrder& modify,
                                          const std::string& native_symbol) {
    if (!enabled_ || !signer_) {
        spdlog::error("[Heimdall] HyperliquidOrderAdapter: disabled, cannot modify order");
        return;
    }

    // Hyperliquid supports order amendment
    try {
        OrderSignParams params;
        params.coin = native_symbol;
        params.is_buy = true;  // side is not in ModifyOrder; retrieved from state in production
        params.price = static_cast<double>(modify.newPrice()) / kScale;
        params.size = static_cast<double>(modify.newQuantity()) / kScale;
        params.cloid = modify.orderId();

        auto tx = signer_->sign_order(params);

        json::object action;
        action["type"] = "modify";

        json::object ord;
        json::object asset;
        asset["coin"] = native_symbol;
        ord["oid"] = modify.orderId();
        ord["order"] = json::object{{"a", asset},
                                    {"b", true},
                                    {"p", std::to_string(params.price)},
                                    {"s", std::to_string(params.size)},
                                    {"r", false},
                                    {"t", json::object{{"limit", json::object{{"tif", "Gtc"}}}}}};
        action["modifies"] = json::array{ord};

        json::object signature;
        signature["r"] = "0x" + tx.r;
        signature["s"] = "0x" + tx.s;
        signature["v"] = tx.v;

        json::object req;
        req["action"] = std::move(action);
        req["nonce"] = tx.nonce;
        req["signature"] = std::move(signature);

        std::string resp = https_post("/exchange", json::serialize(req));
        spdlog::debug("[Heimdall] HyperliquidOrderAdapter: modify resp={}", resp);
    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] HyperliquidOrderAdapter: send_modify failed: {}", e.what());
    }
}

AccountSnapshotData HyperliquidOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    const uint64_t ts_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());

    AccountSnapshotData snap;
    snap.exchange_id = bifrost::protocol::ExchangeId::HYPERLIQUID;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns = ts_ns;

    if (!enabled_) {
        spdlog::warn("[Heimdall] HyperliquidOrderAdapter: disabled — returning empty snapshot");
        return snap;
    }

    // Wallet address required for clearinghouseState query.
    if (wallet_address_.empty()) {
        spdlog::warn(
            "[Heimdall] HyperliquidOrderAdapter: wallet_address not set — "
            "returning empty account snapshot");
        return snap;
    }
    const std::string& wallet_address = wallet_address_;

    // POST /info {type: clearinghouseState, user: <address>} — public, no signing.
    json::object req_body;
    req_body["type"] = "clearinghouseState";
    req_body["user"] = wallet_address;
    std::string resp = https_post("/info", json::serialize(json::value(req_body)));

    try {
        auto j = json::parse(resp).as_object();

        // Available and total balance from marginSummary.
        if (j.contains("marginSummary") && j.at("marginSummary").is_object()) {
            const auto& ms = j.at("marginSummary").as_object();
            if (ms.contains("accountValue"))
                snap.total_equity_e8 = static_cast<int64_t>(
                    std::round(std::stod(std::string(ms.at("accountValue").as_string())) * 1e8));
        }
        if (j.contains("withdrawable"))
            snap.available_balance_e8 = static_cast<int64_t>(
                std::round(std::stod(std::string(j.at("withdrawable").as_string())) * 1e8));

        // Positions from assetPositions.
        if (j.contains("assetPositions") && j.at("assetPositions").is_array()) {
            for (const auto& ap_val : j.at("assetPositions").as_array()) {
                if (!ap_val.is_object()) continue;
                const auto& ap = ap_val.as_object();
                if (!ap.contains("position") || !ap.at("position").is_object()) continue;
                const auto& pos = ap.at("position").as_object();
                if (!pos.contains("szi")) continue;
                const double szi = std::stod(std::string(pos.at("szi").as_string()));
                if (szi == 0.0) continue;

                AccountPosition p;
                if (pos.contains("coin"))
                    p.exchange_symbol = std::string(pos.at("coin").as_string());
                p.net_qty_e8 = static_cast<int64_t>(std::round(szi * 1e8));
                if (pos.contains("entryPx") && pos.at("entryPx").is_string())
                    p.avg_entry_price_e8 = static_cast<int64_t>(
                        std::round(std::stod(std::string(pos.at("entryPx").as_string())) * 1e8));
                if (pos.contains("unrealizedPnl") && pos.at("unrealizedPnl").is_string())
                    p.unrealized_pnl_e8 = static_cast<int64_t>(std::round(
                        std::stod(std::string(pos.at("unrealizedPnl").as_string())) * 1e8));
                snap.positions.push_back(std::move(p));
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] HyperliquidOrderAdapter: failed to parse account snapshot: {}",
                      e.what());
    }

    spdlog::info(
        "[Heimdall] HyperliquidOrderAdapter: account snapshot fetched — balance={:.2f} "
        "positions={}",
        static_cast<double>(snap.available_balance_e8) / 1e8, snap.positions.size());
    return snap;
}

}  // namespace heimdall::adapter
