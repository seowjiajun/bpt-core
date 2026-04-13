#include "heimdall/adapter/okx/okx_order_adapter.h"

#include "heimdall/adapter/common/credentials.h"

#include <bifrost_protocol/ExchangeId.h>
#include <bifrost_protocol/ExecStatus.h>
#include <bifrost_protocol/FeeCurrency.h>
#include <bifrost_protocol/OrderSide.h>
#include <bifrost_protocol/OrderType.h>
#include <bifrost_protocol/RejectReason.h>

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
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdexcept>
#include <string>

namespace heimdall::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

static constexpr double kPriceScale = 1e8;
static constexpr double kQtyScale = 1e5;

// Base64 encode
static std::string base64_encode(const unsigned char* data, std::size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);

    BUF_MEM* buf;
    BIO_get_mem_ptr(b64, &buf);
    std::string out(buf->data, buf->length);
    BIO_free_all(b64);
    return out;
}

static std::string hmac_sha256_b64(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()),
         static_cast<int>(data.size()),
         digest,
         &digest_len);
    return base64_encode(digest, digest_len);
}

OKXOrderAdapter::OKXOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg),
      api_key_(creds.api_key),
      secret_key_(creds.secret_key),
      passphrase_(creds.passphrase) {
    uint32_t epoch_s = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", epoch_s);
    session_prefix_ = std::string(buf, 8);

    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            ygg::log::error("[OKX] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };
}

std::string OKXOrderAdapter::https_request(const std::string& method,
                                           const std::string& path,
                                           const std::string& /*body*/,
                                           bool /*signed_req*/) {
    namespace http = boost::beast::http;
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), cfg_.rest_host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    auto results = resolver.resolve(cfg_.rest_host, cfg_.rest_port);
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(boost::asio::ssl::stream_base::client);

    http::request<http::string_body> req(method == "GET" ? http::verb::get : http::verb::post, path, 11);
    req.set(http::field::host, cfg_.rest_host);
    req.set(http::field::user_agent, "heimdall/0.1");
    if (cfg_.testnet)
        req.set("x-simulated-trading", "1");

    http::write(stream, req);
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    return res.body();
}

void OKXOrderAdapter::fetch_inst_id_codes() {
    // Fetch instIdCodes for all instrument types we might trade.
    const std::vector<std::string> inst_types = {"SPOT", "SWAP", "FUTURES", "MARGIN"};
    for (const auto& inst_type : inst_types) {
        try {
            std::string resp = https_request("GET", "/api/v5/public/instruments?instType=" + inst_type);
            auto root = json::parse(resp);
            if (!root.is_object())
                continue;
            auto data_it = root.as_object().find("data");
            if (data_it == root.as_object().end() || !data_it->value().is_array())
                continue;
            bool is_contract_type = inst_type == "SWAP" || inst_type == "FUTURES" || inst_type == "OPTION";
            for (const auto& item : data_it->value().as_array()) {
                const auto& d = item.as_object();
                auto id_it = d.find("instId");
                auto code_it = d.find("instIdCode");
                if (id_it != d.end() && code_it != d.end()) {
                    std::string inst_id = std::string(id_it->value().as_string());
                    int64_t code = code_it->value().is_int64() ? code_it->value().as_int64()
                                                               : std::stoll(std::string(code_it->value().as_string()));
                    inst_id_codes_[inst_id] = code;

                    // ctVal: base currency per contract for SWAP/FUTURES.
                    // SPOT/MARGIN: sz is in base currency, treat as ctVal=1.
                    double ctval = 1.0;
                    if (is_contract_type) {
                        auto ctval_it = d.find("ctVal");
                        if (ctval_it != d.end() && ctval_it->value().is_string()) {
                            std::string sv = std::string(ctval_it->value().as_string());
                            if (!sv.empty())
                                ctval = std::stod(sv);
                        }
                    }
                    contract_sizes_[inst_id] = ctval;
                }
            }
        } catch (const std::exception& e) {
            ygg::log::warn("[Heimdall] OKXOrderAdapter: fetch_inst_id_codes({}) failed: {}", inst_type, e.what());
        }
    }
    ygg::log::info("[Heimdall] OKXOrderAdapter: loaded {} instIdCodes, {} contract sizes from REST",
                   inst_id_codes_.size(),
                   contract_sizes_.size());
    parser_.set_contract_sizes(contract_sizes_);
}

void OKXOrderAdapter::start() {
    // instIdCodes are only available from the real OKX REST API — skip in backtest
    // (use_tls=false means we're talking to a local simulation server).
    if (cfg_.use_tls) {
        fetch_inst_id_codes();
        fetch_and_log_account_config();
    }
    OrderAdapterBase::start();
}

std::string OKXOrderAdapter::build_login_msg() const {
    uint64_t ts_s = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    std::string ts_str = std::to_string(ts_s);
    std::string prehash = ts_str + "GET" + "/users/self/verify";
    std::string sign = hmac_sha256_b64(secret_key_, prehash);

    json::object login_msg;
    login_msg["op"] = "login";
    json::array args;
    json::object arg;
    arg["apiKey"] = api_key_;
    arg["passphrase"] = passphrase_;
    arg["timestamp"] = ts_str;
    arg["sign"] = sign;
    args.push_back(arg);
    login_msg["args"] = std::move(args);
    return json::serialize(login_msg);
}

void OKXOrderAdapter::handle_message(const std::string& payload, uint64_t recv_ns) {
    ygg::log::info("[Heimdall] OKXOrderAdapter WS rx: {}", payload.substr(0, 500));
    auto root = json::parse(payload);
    if (!root.is_object())
        return;
    const auto& obj = root.as_object();

    // Event messages: login, subscribe acks, errors
    if (auto eit = obj.find("event"); eit != obj.end()) {
        std::string event = std::string(eit->value().as_string());
        if (event == "error") {
            std::string code, msg;
            if (auto cit = obj.find("code"); cit != obj.end())
                code = std::string(cit->value().as_string());
            if (auto mit = obj.find("msg"); mit != obj.end())
                msg = std::string(mit->value().as_string());
            ygg::log::error("[Heimdall] OKXOrderAdapter: error event code={} msg={}", code, msg);
        } else {
            ygg::log::info("[Heimdall] OKXOrderAdapter: event={}", event);
        }
        if (event == "login") {
            ygg::log::info("[Heimdall] OKXOrderAdapter: login successful");
            logged_in_.store(true, std::memory_order_release);
            json::object sub_msg;
            sub_msg["op"] = "subscribe";
            json::array args;
            json::object arg;
            arg["channel"] = "orders";
            arg["instType"] = "ANY";
            args.push_back(arg);
            sub_msg["args"] = std::move(args);
            std::lock_guard<std::mutex> lk(send_mu_);
            if (ws_send_)
                ws_send_(json::serialize(sub_msg));
        }
        return;
    }

    // Op responses: order-placement acks {"op":"order","data":[...]}
    if (auto op_it = obj.find("op"); op_it != obj.end()) {
        if (std::string(op_it->value().as_string()) == "order") {
            auto data_it = obj.find("data");
            if (data_it == obj.end() || !data_it->value().is_array())
                return;
            for (const auto& item : data_it->value().as_array())
                parser_.handle_order_ack(item.as_object(), recv_ns);
        }
        return;
    }

    // Channel push: orders channel fills/state changes
    auto arg_it = obj.find("arg");
    auto data_it = obj.find("data");
    if (arg_it == obj.end() || data_it == obj.end())
        return;
    if (!data_it->value().is_array() || data_it->value().as_array().empty())
        return;

    std::string channel = std::string(arg_it->value().as_object().at("channel").as_string());
    if (channel == "orders") {
        for (const auto& item : data_it->value().as_array())
            parser_.handle_orders_channel_item(item.as_object(), recv_ns);
    }
}

void OKXOrderAdapter::connect_and_run() {
    logged_in_.store(false, std::memory_order_relaxed);
    parser_.reset();

    ygg::log::info("[Heimdall] OKXOrderAdapter connecting {}:{}{} (tls={})",
                   cfg_.ws_host,
                   cfg_.ws_port,
                   cfg_.ws_path,
                   cfg_.use_tls);

    // Generic WS message loop — works for both TLS and plain-TCP stream types.
    auto run_loop = [&](auto& ws) {
        {
            std::lock_guard<std::mutex> lk(send_mu_);
            ws_send_ = [&ws](const std::string& msg) {
                ws.write(net::buffer(msg));
            };
        }

        ws.write(net::buffer(build_login_msg()));

        connected_.store(true, std::memory_order_relaxed);
        ygg::log::info("[Heimdall] OKXOrderAdapter connected, waiting for login");

        auto last_ping = std::chrono::steady_clock::now();
        try {
            beast::flat_buffer buf;
            while (!stop_flag_.load(std::memory_order_relaxed)) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_ping >= std::chrono::seconds(15)) {
                    {
                        std::lock_guard<std::mutex> lk(send_mu_);
                        if (ws_send_)
                            ws_send_("ping");
                    }
                    last_ping = now;
                }

                beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
                beast::error_code ec;
                ws.read(buf, ec);

                if (ec == beast::error::timeout) {
                    buf.consume(buf.size());
                    continue;
                }
                if (ec)
                    throw beast::system_error(ec);

                uint64_t recv_ns = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
                std::string payload(static_cast<const char*>(buf.data().data()), buf.data().size());
                buf.consume(buf.size());

                if (payload == "ping") {
                    std::lock_guard<std::mutex> lk(send_mu_);
                    if (ws_send_)
                        ws_send_("pong");
                    continue;
                }
                if (payload == "pong")
                    continue;

                handle_message(payload, recv_ns);
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
        beast::error_code ec;
        ws.close(websocket::close_code::normal, ec);
    };

    tcp::resolver resolver(ioc_);

    if (!cfg_.use_tls) {
        // Plain TCP — used when connecting to a local simulation server (backtest).
        websocket::stream<beast::tcp_stream> ws(ioc_);
        auto results = resolver.resolve(cfg_.ws_host, cfg_.ws_port);
        beast::get_lowest_layer(ws).connect(results);
        ws.text(true);
        ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) { req.set(boost::beast::http::field::user_agent, "heimdall/0.1"); }));
        ws.handshake(cfg_.ws_host, cfg_.ws_path);
        ws.set_option(websocket::stream_base::timeout{std::chrono::seconds(15), websocket::stream_base::none(), false});
        run_loop(ws);
        return;
    }

    // TLS path — production.
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc_, ssl_ctx_);
    auto results = resolver.resolve(cfg_.ws_host, cfg_.ws_port);
    beast::get_lowest_layer(ws).connect(results);

    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), cfg_.ws_host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    ws.next_layer().handshake(ssl::stream_base::client);

    ws.text(true);
    ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) { req.set(boost::beast::http::field::user_agent, "heimdall/0.1"); }));
    ws.handshake(cfg_.ws_host, cfg_.ws_path);
    ws.set_option(websocket::stream_base::timeout{std::chrono::seconds(15), websocket::stream_base::none(), false});
    run_loop(ws);
}

void OKXOrderAdapter::send_new_order(const bifrost::protocol::NewOrder& order) {
    using OT = bifrost::protocol::OrderType;
    using OS = bifrost::protocol::OrderSide;
    using TIF = bifrost::protocol::TimeInForce;

    const std::string exchange_symbol = order.getExchangeSymbolAsString();

    std::string cloid = session_prefix_ + "G" + std::to_string(order.orderId());
    parser_.register_order(cloid, order.orderId());

    std::string side_str = (order.side() == OS::BUY) ? "buy" : "sell";
    std::string type_str;
    if (order.orderType() == OT::MARKET)
        type_str = "market";
    else if (order.orderType() == OT::POST_ONLY)
        type_str = "post_only";
    else
        type_str = "limit";

    std::string tif_str;
    switch (order.timeInForce()) {
        case TIF::IOC:
            tif_str = "ioc";
            break;
        case TIF::FOK:
            tif_str = "fok";
            break;
        default:
            tif_str = "gtc";
            break;
    }

    json::object arg;
    arg["instId"] = exchange_symbol;
    // wseeapap endpoint requires instIdCode
    if (auto it = inst_id_codes_.find(exchange_symbol); it != inst_id_codes_.end())
        arg["instIdCode"] = it->second;
    // Perpetuals/futures use cross margin; spot uses cash.
    bool is_perp =
        exchange_symbol.find("-SWAP") != std::string::npos || exchange_symbol.find("-FUTURES") != std::string::npos;
    arg["tdMode"] = is_perp ? "cross" : "cash";
    arg["side"] = side_str;
    arg["ordType"] = type_str;
    {
        // Convert Fenrir qty (base_currency * kQtyScale) → OKX sz (contracts).
        // SWAP/FUTURES: sz = qty_base / ctVal.  SPOT: ctVal=1, sz = qty_base.
        double qty_base = static_cast<double>(order.quantity()) / kQtyScale;
        double ctval = 1.0;
        if (auto it = contract_sizes_.find(exchange_symbol); it != contract_sizes_.end())
            ctval = it->second;
        arg["sz"] = std::to_string(qty_base / ctval);
    }
    arg["clOrdId"] = cloid;
    if (order.orderType() != OT::MARKET) {
        arg["px"] = std::to_string(static_cast<double>(order.price()) / kPriceScale);
    }

    json::object msg;
    msg["id"] = "r" + std::to_string(ws_req_id_.fetch_add(1, std::memory_order_relaxed));
    msg["op"] = "order";
    json::array args;
    args.push_back(arg);
    msg["args"] = std::move(args);

    std::string frame = json::serialize(msg);
    auto emit_rejection = [&]() {
        uint64_t ts = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        ExecEvent rej;
        rej.order_id = order.orderId();
        rej.exchange_id = bifrost::protocol::ExchangeId::OKX;
        rej.instrument_id = 0;
        rej.local_ts_ns = ts;
        rej.exchange_ts_ns = ts;
        rej.side = (order.side() == bifrost::protocol::OrderSide::BUY) ? bifrost::protocol::OrderSide::BUY
                                                                       : bifrost::protocol::OrderSide::SELL;
        rej.order_type = order.orderType();
        rej.status = bifrost::protocol::ExecStatus::REJECTED;
        rej.reject_reason = bifrost::protocol::RejectReason::EXCHANGE_ERROR;
        if (!exec_queue_.try_push(rej))
            ygg::log::error("[OKX] exec_queue full — dropped rejection order_id={}", rej.order_id);
    };

    std::lock_guard<std::mutex> lk(send_mu_);
    if (!ws_send_) {
        ygg::log::warn(
            "[Heimdall] OKXOrderAdapter: send_new_order: WS not connected, "
            "rejecting order={}",
            order.orderId());
        emit_rejection();
        return;
    }
    try {
        ws_send_(frame);
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] OKXOrderAdapter: send_new_order failed: {}", e.what());
        emit_rejection();
    }
}

void OKXOrderAdapter::send_cancel(const bifrost::protocol::CancelOrder& cancel, const std::string& native_symbol) {
    std::string cloid = session_prefix_ + "G" + std::to_string(cancel.orderId());

    json::object arg;
    arg["instId"] = native_symbol;
    arg["clOrdId"] = cloid;

    json::object msg;
    msg["id"] = "c" + std::to_string(ws_req_id_.fetch_add(1, std::memory_order_relaxed));
    msg["op"] = "cancel-order";
    json::array args;
    args.push_back(arg);
    msg["args"] = std::move(args);

    std::string frame = json::serialize(msg);
    std::lock_guard<std::mutex> lk(send_mu_);
    if (ws_send_) {
        try {
            ws_send_(frame);
        } catch (const std::exception& e) {
            ygg::log::error("[Heimdall] OKXOrderAdapter: send_cancel failed: {}", e.what());
        }
    }
}

void OKXOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    ygg::log::warn("[Heimdall] OKXOrderAdapter: send_cancel_all called instrument_id={}", instrument_id);
}

void OKXOrderAdapter::send_modify(const bifrost::protocol::ModifyOrder& modify, const std::string& native_symbol) {
    std::string cloid = session_prefix_ + "G" + std::to_string(modify.orderId());

    json::object arg;
    arg["instId"] = native_symbol;
    arg["clOrdId"] = cloid;
    arg["newPx"] = std::to_string(static_cast<double>(modify.newPrice()) / kPriceScale);
    {
        double qty_base = static_cast<double>(modify.newQuantity()) / kQtyScale;
        double ctval = 1.0;
        if (auto it = contract_sizes_.find(native_symbol); it != contract_sizes_.end())
            ctval = it->second;
        arg["newSz"] = std::to_string(qty_base / ctval);
    }

    json::object msg;
    msg["op"] = "amend-order";
    json::array args;
    args.push_back(arg);
    msg["args"] = std::move(args);

    std::string frame = json::serialize(msg);
    std::lock_guard<std::mutex> lk(send_mu_);
    if (ws_send_) {
        try {
            ws_send_(frame);
        } catch (const std::exception& e) {
            ygg::log::error("[Heimdall] OKXOrderAdapter: send_modify failed: {}", e.what());
        }
    }
}

// Build OKX REST auth headers for a GET request (no body).
// prehash = timestamp_s + "GET" + path
static boost::beast::http::request<boost::beast::http::string_body> okx_signed_get(const std::string& host,
                                                                                   const std::string& path,
                                                                                   const std::string& api_key,
                                                                                   const std::string& secret_key,
                                                                                   const std::string& passphrase,
                                                                                   bool testnet) {
    namespace http = boost::beast::http;
    const uint64_t ts_s = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const std::string ts_str = std::to_string(ts_s);
    const std::string prehash = ts_str + "GET" + path;
    const std::string sign = hmac_sha256_b64(secret_key, prehash);

    http::request<http::string_body> req(http::verb::get, path, 11);
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "heimdall/0.1");
    req.set("OK-ACCESS-KEY", api_key);
    req.set("OK-ACCESS-SIGN", sign);
    req.set("OK-ACCESS-TIMESTAMP", ts_str);
    req.set("OK-ACCESS-PASSPHRASE", passphrase);
    if (testnet)
        req.set("x-simulated-trading", "1");
    return req;
}

AccountSnapshotData OKXOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    namespace http = boost::beast::http;
    using tcp = net::ip::tcp;

    const uint64_t ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    auto do_get = [&](const std::string& path) -> std::string {
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

        auto req = okx_signed_get(cfg_.rest_host, path, api_key_, secret_key_, passphrase_, cfg_.testnet);
        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        return res.body();
    };

    AccountSnapshotData snap;
    snap.exchange_id = bifrost::protocol::ExchangeId::OKX;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns = ts_ns;

    try {
        // Balance endpoint — totalEq and availBal.
        auto bal_resp = do_get("/api/v5/account/balance");
        auto bal_j = json::parse(bal_resp);
        if (bal_j.is_object() && bal_j.as_object().contains("data") && bal_j.as_object().at("data").is_array()) {
            const auto& data = bal_j.as_object().at("data").as_array();
            if (!data.empty() && data[0].is_object()) {
                const auto& d = data[0].as_object();
                if (d.contains("totalEq"))
                    snap.total_equity_e8 =
                        static_cast<int64_t>(std::round(std::stod(std::string(d.at("totalEq").as_string())) * 1e8));
                // Sum availBal across USDT details.
                if (d.contains("details") && d.at("details").is_array()) {
                    for (const auto& detail : d.at("details").as_array()) {
                        if (!detail.is_object())
                            continue;
                        const auto& de = detail.as_object();
                        if (de.contains("ccy") && std::string(de.at("ccy").as_string()) == "USDT" &&
                            de.contains("availBal"))
                            snap.available_balance_e8 = static_cast<int64_t>(
                                std::round(std::stod(std::string(de.at("availBal").as_string())) * 1e8));
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        ygg::log::warn("[Heimdall] OKXOrderAdapter: failed to fetch balance: {}", e.what());
    }

    try {
        // Positions endpoint.
        auto pos_resp = do_get("/api/v5/account/positions");
        auto pos_j = json::parse(pos_resp);
        if (pos_j.is_object() && pos_j.as_object().contains("data") && pos_j.as_object().at("data").is_array()) {
            for (const auto& p : pos_j.as_object().at("data").as_array()) {
                if (!p.is_object())
                    continue;
                const auto& po = p.as_object();
                if (!po.contains("pos"))
                    continue;
                const double pos_qty = std::stod(std::string(po.at("pos").as_string()));
                if (pos_qty == 0.0)
                    continue;

                AccountPosition ap;
                if (po.contains("instId"))
                    ap.exchange_symbol = std::string(po.at("instId").as_string());
                ap.net_qty_e8 = static_cast<int64_t>(std::round(pos_qty * 1e8));
                if (po.contains("avgPx") && !po.at("avgPx").as_string().empty())
                    ap.avg_entry_price_e8 =
                        static_cast<int64_t>(std::round(std::stod(std::string(po.at("avgPx").as_string())) * 1e8));
                if (po.contains("upl") && !po.at("upl").as_string().empty())
                    ap.unrealized_pnl_e8 =
                        static_cast<int64_t>(std::round(std::stod(std::string(po.at("upl").as_string())) * 1e8));
                snap.positions.push_back(std::move(ap));
            }
        }
    } catch (const std::exception& e) {
        ygg::log::warn("[Heimdall] OKXOrderAdapter: failed to fetch positions: {}", e.what());
    }

    ygg::log::info("[Heimdall] OKXOrderAdapter: account snapshot fetched — balance={:.2f} positions={}",
                   static_cast<double>(snap.available_balance_e8) / 1e8,
                   snap.positions.size());
    return snap;
}

void OKXOrderAdapter::fetch_and_log_account_config() {
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    namespace http = boost::beast::http;
    using tcp = net::ip::tcp;

    try {
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

        auto req = okx_signed_get(cfg_.rest_host,
                                  "/api/v5/account/config",
                                  api_key_,
                                  secret_key_,
                                  passphrase_,
                                  cfg_.testnet);
        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);

        auto j = json::parse(res.body());
        if (!j.is_object()) {
            ygg::log::warn("[OKX account/config] unexpected response shape");
            return;
        }
        auto& obj = j.as_object();
        if (obj.contains("code") && obj.at("code").as_string() != "0") {
            ygg::log::warn("[OKX account/config] error code={} msg={}",
                           std::string(obj.at("code").as_string()),
                           obj.contains("msg") ? std::string(obj.at("msg").as_string()) : "");
            return;
        }
        if (!obj.contains("data") || !obj.at("data").is_array() || obj.at("data").as_array().empty())
            return;

        const auto& d = obj.at("data").as_array().at(0).as_object();
        const std::string acct_lv = d.contains("acctLv") ? std::string(d.at("acctLv").as_string()) : "?";
        const std::string perm = d.contains("perm") ? std::string(d.at("perm").as_string()) : "?";
        const std::string pos_mode = d.contains("posMode") ? std::string(d.at("posMode").as_string()) : "?";
        const std::string label = d.contains("label") ? std::string(d.at("label").as_string()) : "";
        const std::string uid = d.contains("uid") ? std::string(d.at("uid").as_string()) : "";

        // Human-readable account level mapping per OKX docs:
        // 1 = Simple (spot only), 2 = Single-currency margin,
        // 3 = Multi-currency margin, 4 = Portfolio margin.
        const char* lvl_name = "Unknown";
        bool derivatives_allowed = false;
        if (acct_lv == "1") {
            lvl_name = "Simple (spot only)";
        } else if (acct_lv == "2") {
            lvl_name = "Single-currency margin";
            derivatives_allowed = true;
        } else if (acct_lv == "3") {
            lvl_name = "Multi-currency margin";
            derivatives_allowed = true;
        } else if (acct_lv == "4") {
            lvl_name = "Portfolio margin";
            derivatives_allowed = true;
        }

        ygg::log::info("[OKX account/config] uid={} label='{}' acctLv={} ({}) perm={} posMode={}",
                       uid, label, acct_lv, lvl_name, perm, pos_mode);

        if (!derivatives_allowed) {
            ygg::log::warn("[OKX account/config] ACCOUNT CANNOT TRADE DERIVATIVES — "
                           "level {} is spot-only. Perp/futures/options orders will reject "
                           "with sCode=51155 'local compliance requirements' (the real cause "
                           "is the account level, not geo-compliance). Upgrade via OKX UI: "
                           "Demo Trading → Account Mode → Single-currency margin or higher.",
                           acct_lv);
        }
    } catch (const std::exception& e) {
        ygg::log::warn("[OKX account/config] fetch failed: {}", e.what());
    }
}

}  // namespace heimdall::adapter
