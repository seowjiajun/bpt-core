#include "heimdall/adapter/binance/binance_order_adapter.h"

#include <bifrost_protocol/ExchangeId.h>
#include <bifrost_protocol/ExecStatus.h>
#include <bifrost_protocol/FeeCurrency.h>
#include <bifrost_protocol/OrderSide.h>
#include <bifrost_protocol/OrderType.h>
#include <bifrost_protocol/RejectReason.h>
#include <bifrost_protocol/TimeInForce.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
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
#include <iomanip>
#include <sstream>
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

// HMAC-SHA256 hex encoding
static std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()), digest,
         &digest_len);

    std::ostringstream oss;
    for (unsigned int i = 0; i < digest_len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return oss.str();
}

static bifrost::protocol::FeeCurrency::Value parse_fee_currency(const std::string& asset) {
    using FC = bifrost::protocol::FeeCurrency;
    if (asset == "BTC") return FC::BTC;
    if (asset == "ETH") return FC::ETH;
    if (asset == "BNB") return FC::BNB;
    if (asset == "USDT") return FC::USDT;
    return FC::USDT;
}

BinanceOrderAdapter::BinanceOrderAdapter(const config::AdapterConfig& cfg,
                                         const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg), api_key_(creds.api_key), secret_key_(creds.secret_key) {
    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            spdlog::error("[Binance] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };
}

std::string BinanceOrderAdapter::sign_query(const std::string& params) const {
    uint64_t ts_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count());
    std::string full = params + "&timestamp=" + std::to_string(ts_ms);
    std::string sig = hmac_sha256_hex(secret_key_, full);
    return full + "&signature=" + sig;
}

std::string BinanceOrderAdapter::https_request(const std::string& method, const std::string& path,
                                               const std::string& body, bool with_api_key) {
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

    http::verb verb;
    if (method == "POST")
        verb = http::verb::post;
    else if (method == "PUT")
        verb = http::verb::put;
    else if (method == "DELETE")
        verb = http::verb::delete_;
    else
        verb = http::verb::get;

    http::request<http::string_body> req(verb, path, 11);
    req.set(http::field::host, cfg_.rest_host);
    req.set(http::field::user_agent, "heimdall/0.1");
    req.set(http::field::content_type, "application/x-www-form-urlencoded");
    if (with_api_key) req.set("X-MBX-APIKEY", api_key_);
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

std::string BinanceOrderAdapter::create_listen_key() {
    std::string resp = https_request("POST", "/api/v3/userDataStream", "", true);
    try {
        auto root = json::parse(resp);
        if (!root.is_object()) return "";
        if (root.as_object().contains("listenKey"))
            return std::string(root.as_object().at("listenKey").as_string());
    } catch (const std::exception&) {
        // Endpoint gone (e.g. testnet 410) — REST-only mode
    }
    return "";
}

void BinanceOrderAdapter::extend_listen_key(const std::string& listen_key) {
    https_request("PUT", "/api/v3/userDataStream?listenKey=" + listen_key, "", true);
}

void BinanceOrderAdapter::delete_listen_key(const std::string& listen_key) {
    https_request("DELETE", "/api/v3/userDataStream?listenKey=" + listen_key, "", true);
}

void BinanceOrderAdapter::handle_user_data_message(const std::string& payload, uint64_t recv_ns) {
    auto root = json::parse(payload);
    if (!root.is_object()) return;
    parser_.handle_execution_report(root.as_object(), recv_ns);
}

void BinanceOrderAdapter::connect_and_run() {
    spdlog::info("[Heimdall] BinanceOrderAdapter: creating listen key");
    listen_key_ = create_listen_key();
    if (listen_key_.empty()) {
        // Listen key endpoint gone (e.g. testnet 410) — fall back to REST-only
        // mode. ExecReports are emitted synchronously from send_new_order REST
        // responses.
        spdlog::warn(
            "[Heimdall] BinanceOrderAdapter: no listen key — REST-only "
            "mode (exec reports from order response)");
        connected_.store(true, std::memory_order_relaxed);
        while (!stop_flag_.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::seconds(1));
        return;
    }

    spdlog::info("[Heimdall] BinanceOrderAdapter: connecting WS for user data stream");

    const std::string ws_path = cfg_.ws_path + "/" + listen_key_;

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
    ws->handshake(cfg_.ws_host, ws_path);

    connected_.store(true, std::memory_order_relaxed);
    spdlog::info("[Heimdall] BinanceOrderAdapter: connected");

    last_ping_ = std::chrono::steady_clock::now();

    beast::flat_buffer buf;
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Set a read timeout so we can periodically ping the listen key
        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(30));

        beast::error_code ec;
        ws->read(buf, ec);

        if (ec == beast::error::timeout) {
            // Ping listen key every 30 minutes
            auto now = std::chrono::steady_clock::now();
            if (now - last_ping_ >= std::chrono::minutes(30)) {
                extend_listen_key(listen_key_);
                last_ping_ = now;
            }
            buf.consume(buf.size());
            continue;
        }
        if (ec) throw beast::system_error(ec);

        uint64_t recv_ns =
            static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        handle_user_data_message(
            std::string(static_cast<const char*>(buf.data().data()), buf.data().size()), recv_ns);
        buf.consume(buf.size());
    }

    delete_listen_key(listen_key_);
    ws->close(websocket::close_code::normal);
}

void BinanceOrderAdapter::send_new_order(const bifrost::protocol::NewOrder& order) {
    using OT = bifrost::protocol::OrderType;
    using OS = bifrost::protocol::OrderSide;
    using TIF = bifrost::protocol::TimeInForce;

    const std::string exchange_symbol = order.getExchangeSymbolAsString();

    std::string cloid = "G" + std::to_string(order.orderId());
    parser_.register_order(cloid, order.orderId());

    std::string side_str = (order.side() == OS::BUY) ? "BUY" : "SELL";

    std::string type_str;
    if (order.orderType() == OT::MARKET)
        type_str = "MARKET";
    else if (order.orderType() == OT::POST_ONLY)
        type_str = "LIMIT_MAKER";
    else
        type_str = "LIMIT";

    std::string tif_str;
    switch (order.timeInForce()) {
        case TIF::IOC:
            tif_str = "IOC";
            break;
        case TIF::FOK:
            tif_str = "FOK";
            break;
        default:
            tif_str = "GTC";
            break;
    }

    std::string params =
        "symbol=" + exchange_symbol + "&side=" + side_str + "&type=" + type_str +
        "&quantity=" + std::to_string(static_cast<double>(order.quantity()) / kScale) +
        "&newClientOrderId=" + cloid;

    if (order.orderType() != OT::MARKET) {
        params += "&timeInForce=" + tif_str;
        params += "&price=" + std::to_string(static_cast<double>(order.price()) / kScale);
    }

    std::string signed_params = sign_query(params);

    try {
        std::string resp = https_request("POST", "/api/v3/order?" + signed_params, "", true);
        spdlog::debug("[Heimdall] BinanceOrderAdapter: new order resp={}", resp);

        uint64_t recv_ns =
            static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());

        auto root = json::parse(resp);
        if (!root.is_object()) return;
        const auto& obj = root.as_object();

        using ES = bifrost::protocol::ExecStatus;
        using OS = bifrost::protocol::OrderSide;
        using OT = bifrost::protocol::OrderType;
        using RR = bifrost::protocol::RejectReason;

        ExecEvent ev;
        ev.order_id = order.orderId();
        ev.exchange_id = bifrost::protocol::ExchangeId::BINANCE;
        ev.instrument_id = 0;
        ev.local_ts_ns = recv_ns;
        ev.reject_reason = RR::OK;

        // Binance error response has "code" field (negative integer)
        if (auto code_it = obj.find("code"); code_it != obj.end()) {
            auto msg_it = obj.find("msg");
            std::string msg =
                (msg_it != obj.end()) ? std::string(msg_it->value().as_string()) : "?";
            spdlog::error(
                "[Heimdall] BinanceOrderAdapter: exchange rejected order={} "
                "code={} msg={}",
                order.orderId(), code_it->value().as_int64(), msg);
            ExecEvent rej;
            rej.order_id = order.orderId();
            rej.exchange_id = bifrost::protocol::ExchangeId::BINANCE;
            rej.instrument_id = 0;
            rej.local_ts_ns = recv_ns;
            rej.exchange_ts_ns = recv_ns;
            rej.side = (order.side() == OS::BUY) ? OS::BUY : OS::SELL;
            rej.order_type = order.orderType();
            rej.status = ES::REJECTED;
            rej.reject_reason = RR::EXCHANGE_ERROR;
            if (!exec_queue_.try_push(rej))
                spdlog::error("[Binance] exec_queue full — dropped rejection order_id={}", rej.order_id);
            return;
        }

        if (auto oit = obj.find("orderId"); oit != obj.end())
            ev.exchange_order_id = static_cast<uint64_t>(oit->value().as_int64());

        ev.side = (order.side() == OS::BUY) ? OS::BUY : OS::SELL;
        ev.order_type = order.orderType();

        ev.price =
            static_cast<int64_t>(std::stod(std::string(obj.at("price").as_string())) * kScale);
        ev.filled_qty = static_cast<uint64_t>(
            std::stod(std::string(obj.at("executedQty").as_string())) * kScale);
        uint64_t total_qty =
            static_cast<uint64_t>(std::stod(std::string(obj.at("origQty").as_string())) * kScale);
        ev.remaining_qty = total_qty > ev.filled_qty ? total_qty - ev.filled_qty : 0;

        if (auto tsit = obj.find("transactTime"); tsit != obj.end())
            ev.exchange_ts_ns = static_cast<uint64_t>(tsit->value().as_int64()) * 1000000ULL;
        else
            ev.exchange_ts_ns = recv_ns;

        // Fee: sum fills if present
        ev.fee = 0;
        ev.fee_currency = bifrost::protocol::FeeCurrency::USDT;
        if (auto fit = obj.find("fills"); fit != obj.end() && fit->value().is_array()) {
            for (const auto& fill : fit->value().as_array()) {
                const auto& f = fill.as_object();
                ev.fee += static_cast<int64_t>(
                    std::stod(std::string(f.at("commission").as_string())) * kScale);
                if (auto fcit = f.find("commissionAsset"); fcit != f.end())
                    ev.fee_currency = parse_fee_currency(std::string(fcit->value().as_string()));
            }
        }

        std::string status = std::string(obj.at("status").as_string());
        if (status == "NEW")
            ev.status = ES::ACKED;
        else if (status == "FILLED")
            ev.status = ES::FILLED;
        else if (status == "PARTIALLY_FILLED")
            ev.status = ES::PARTIAL;
        else if (status == "CANCELED" || status == "EXPIRED")
            ev.status = ES::CANCELLED;
        else {
            ev.status = ES::REJECTED;
            ev.reject_reason = RR::EXCHANGE_ERROR;
        }

        if (!exec_queue_.try_push(ev))
            spdlog::error("[Binance] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);

    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] BinanceOrderAdapter: send_new_order failed: {}", e.what());
        // Emit a rejection so the risk open-orders counter is decremented
        uint64_t ts =
            static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        ExecEvent rej;
        rej.order_id = order.orderId();
        rej.exchange_id = bifrost::protocol::ExchangeId::BINANCE;
        rej.instrument_id = 0;
        rej.local_ts_ns = ts;
        rej.exchange_ts_ns = ts;
        rej.side = (order.side() == bifrost::protocol::OrderSide::BUY)
                       ? bifrost::protocol::OrderSide::BUY
                       : bifrost::protocol::OrderSide::SELL;
        rej.order_type = order.orderType();
        rej.status = bifrost::protocol::ExecStatus::REJECTED;
        rej.reject_reason = bifrost::protocol::RejectReason::EXCHANGE_ERROR;
        if (!exec_queue_.try_push(rej))
            spdlog::error("[Binance] exec_queue full — dropped rejection order_id={}", rej.order_id);
    }
}

void BinanceOrderAdapter::send_cancel(const bifrost::protocol::CancelOrder& cancel,
                                      const std::string& native_symbol) {
    std::string cloid = "G" + std::to_string(cancel.orderId());
    std::string params = "symbol=" + native_symbol + "&origClientOrderId=" + cloid;
    std::string signed_params = sign_query(params);

    try {
        std::string resp = https_request("DELETE", "/api/v3/order?" + signed_params, "", true);
        spdlog::debug("[Heimdall] BinanceOrderAdapter: cancel resp={}", resp);
    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] BinanceOrderAdapter: send_cancel failed: {}", e.what());
    }
}

void BinanceOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    (void)instrument_id;
    // CancelAll with instrument_id=0 means all — Binance requires symbol per
    // cancel-all The main loop will call this per-instrument; for now log a
    // warning.
    spdlog::warn(
        "[Heimdall] BinanceOrderAdapter: send_cancel_all called with "
        "instrument_id={}",
        instrument_id);
}

void BinanceOrderAdapter::send_modify(const bifrost::protocol::ModifyOrder& modify,
                                      const std::string& native_symbol) {
    // Binance does not have a native modify — cancel + replace.
    // Cancel existing order, then send a new one.
    std::string cloid = "G" + std::to_string(modify.orderId());
    std::string cancel_params = "symbol=" + native_symbol + "&origClientOrderId=" + cloid;
    std::string signed_cancel = sign_query(cancel_params);

    try {
        https_request("DELETE", "/api/v3/order?" + signed_cancel, "", true);

        // Place new order with updated price/qty
        std::string new_cloid = "G" + std::to_string(modify.orderId()) + "m";
        std::string new_params =
            "symbol=" + native_symbol +
            "&side=BUY" +  // side not in ModifyOrder; must be retrieved from state
            "&type=LIMIT" +
            "&quantity=" + std::to_string(static_cast<double>(modify.newQuantity()) / kScale) +
            "&price=" + std::to_string(static_cast<double>(modify.newPrice()) / kScale) +
            "&timeInForce=GTC" + "&newClientOrderId=" + new_cloid;
        std::string signed_new = sign_query(new_params);
        https_request("POST", "/api/v3/order?" + signed_new, "", true);
    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] BinanceOrderAdapter: send_modify failed: {}", e.what());
    }
}

AccountSnapshotData BinanceOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    using namespace std::chrono;
    const uint64_t ts_ns = static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
    const uint64_t ts_ms = static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());

    // GET /fapi/v2/account — futures/perp account balance and open positions.
    std::string params = "timestamp=" + std::to_string(ts_ms);
    std::string signed_params = sign_query(params);
    std::string resp = https_request("GET", "/fapi/v2/account?" + signed_params, "", true);

    AccountSnapshotData snap;
    snap.exchange_id = bifrost::protocol::ExchangeId::BINANCE;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns = ts_ns;

    try {
        auto root = json::parse(resp);
        if (!root.is_object()) return snap;
        const auto& obj = root.as_object();

        if (obj.contains("availableBalance"))
            snap.available_balance_e8 = static_cast<int64_t>(
                std::round(std::stod(std::string(obj.at("availableBalance").as_string())) * 1e8));
        if (obj.contains("totalWalletBalance"))
            snap.total_equity_e8 = static_cast<int64_t>(
                std::round(std::stod(std::string(obj.at("totalWalletBalance").as_string())) * 1e8));
        if (obj.contains("totalUnrealizedProfit"))
            snap.total_equity_e8 += static_cast<int64_t>(std::round(
                std::stod(std::string(obj.at("totalUnrealizedProfit").as_string())) * 1e8));

        if (obj.contains("positions") && obj.at("positions").is_array()) {
            for (const auto& p : obj.at("positions").as_array()) {
                if (!p.is_object()) continue;
                const auto& po = p.as_object();
                const double pos_amt = std::stod(std::string(po.at("positionAmt").as_string()));
                if (pos_amt == 0.0) continue;

                AccountPosition ap;
                ap.exchange_symbol = std::string(po.at("symbol").as_string());
                ap.net_qty_e8 = static_cast<int64_t>(std::round(pos_amt * 1e8));
                if (po.contains("entryPrice"))
                    ap.avg_entry_price_e8 = static_cast<int64_t>(
                        std::round(std::stod(std::string(po.at("entryPrice").as_string())) * 1e8));
                if (po.contains("unrealizedProfit"))
                    ap.unrealized_pnl_e8 = static_cast<int64_t>(std::round(
                        std::stod(std::string(po.at("unrealizedProfit").as_string())) * 1e8));
                snap.positions.push_back(std::move(ap));
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[Heimdall] BinanceOrderAdapter: failed to parse account snapshot: {}",
                      e.what());
    }

    spdlog::info(
        "[Heimdall] BinanceOrderAdapter: account snapshot fetched — balance={:.2f} positions={}",
        static_cast<double>(snap.available_balance_e8) / 1e8, snap.positions.size());
    return snap;
}

}  // namespace heimdall::adapter
