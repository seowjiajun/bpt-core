#include "heimdall/adapter/hyperliquid/hyperliquid_order_adapter.h"

#include "heimdall/adapter/common/credentials.h"

#include <bifrost_protocol/ExchangeId.h>
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
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace heimdall::adapter {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

static constexpr double kScale = 1e8;

// Format a double matching Hyperliquid's Python SDK `float_to_wire`:
//   1. Format to 8 decimals ("%.8f")
//   2. Strip trailing zeros after the decimal point
//   3. Strip the decimal point if nothing follows
//
// Examples (must match Python):
//   72198.0575 → "72198.0575"
//   50000.0    → "50000"
//   0.001      → "0.001"
//
// HL's server normalizes the string to this canonical form before hashing,
// so the wire string we msgpack MUST match this exactly — otherwise the
// server-computed hash diverges from ours and ECDSA recovers a garbage
// address (yielding "User or API Wallet does not exist").
std::string float_to_wire(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.8f", v);
    std::string s(buf);
    // Strip trailing zeros after the decimal point.
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last == dot)
            s.erase(dot);           // e.g. "50000." → "50000"
        else
            s.erase(last + 1);      // e.g. "72198.05750000" → "72198.0575"
    }
    if (s == "-0") s = "0";
    return s;
}

HyperliquidOrderAdapter::HyperliquidOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg),
      wallet_address_(creds.wallet_address) {
    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            ygg::log::error("[Hyperliquid] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };

    if (creds.private_key.empty()) {
        enabled_ = false;
        ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: disabled — private_key not set");
        return;
    }
    try {
        signer_ = std::make_unique<HyperliquidSigner>(creds.private_key, !cfg.testnet);
        enabled_ = true;
        ygg::log::info("[Heimdall] HyperliquidOrderAdapter: signer loaded");
    } catch (const std::exception& e) {
        enabled_ = false;
        ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: disabled — {}", e.what());
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
    if (!root.is_object())
        return;
    const auto& obj = root.as_object();

    auto channel_it = obj.find("channel");
    auto data_it = obj.find("data");
    if (channel_it == obj.end() || data_it == obj.end())
        return;

    if (std::string(channel_it->value().as_string()) == "user") {
        const auto& data = data_it->value().as_object();
        auto fills_it = data.find("fills");
        if (fills_it == data.end())
            return;
        parser_.handle_fills(fills_it->value().as_array(), recv_ns);
    }
}

void HyperliquidOrderAdapter::connect_and_run() {
    if (!enabled_) {
        ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: running in disabled mode");
        while (!stop_flag_.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::seconds(1));
        return;
    }

    ygg::log::info("[Heimdall] HyperliquidOrderAdapter connecting WS {}:{}{}",
                   cfg_.ws_host,
                   cfg_.ws_port,
                   cfg_.ws_path);

    tcp::resolver resolver(ioc_);
    auto ws = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(ioc_, ssl_ctx_);

    auto results = resolver.resolve(cfg_.ws_host, cfg_.ws_port);
    beast::get_lowest_layer(*ws).connect(results);

    if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), cfg_.ws_host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    ws->next_layer().handshake(ssl::stream_base::client);

    ws->set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) { req.set(boost::beast::http::field::user_agent, "heimdall/0.1"); }));
    ws->handshake(cfg_.ws_host, cfg_.ws_path);

    // Subscribe to userFills for the main wallet. The real address comes
    // from credentials (HYPERLIQUID_WALLET_ADDRESS). A placeholder zero
    // address would cause Hyperliquid to silently reject the subscription,
    // leaving the WS connection idle and closed after ~60s.
    if (wallet_address_.empty()) {
        ygg::log::warn(
            "[Heimdall] HyperliquidOrderAdapter: wallet_address empty — "
            "skipping userFills subscribe. WS will idle-close.");
    } else {
        json::object sub_msg;
        sub_msg["method"] = "subscribe";
        json::object sub_detail;
        sub_detail["type"] = "userFills";
        sub_detail["user"] = wallet_address_;
        sub_msg["subscription"] = sub_detail;
        ws->write(net::buffer(json::serialize(sub_msg)));
        ygg::log::info("[Heimdall] HyperliquidOrderAdapter: subscribed userFills for {}", wallet_address_);
    }

    connected_.store(true, std::memory_order_relaxed);
    ygg::log::info("[Heimdall] HyperliquidOrderAdapter connected");

    // Hyperliquid closes idle WS after ~60s. Previous attempt (ping after
    // ws.read) didn't work: ws->read() blocks for the full 60s even with
    // get_lowest_layer().expires_after(5s) — that timer only applies to the
    // first TCP op inside a multi-frame WS read, so the timeout is bypassed
    // by trickle-data from the initial userFills snapshot.
    //
    // Solution: dedicated ping thread. Beast websocket::stream supports
    // concurrent read+write from different threads as long as each direction
    // is single-threaded. Reader stays in this loop; ping thread writes.
    std::atomic<bool> ping_stop{false};
    std::thread ping_thread([&] {
        while (!ping_stop.load(std::memory_order_relaxed)) {
            // Sleep in 1s slices so shutdown is responsive.
            for (int i = 0; i < 20 && !ping_stop.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (ping_stop.load(std::memory_order_relaxed))
                break;
            try {
                static const std::string msg = R"({"method":"ping"})";
                ws->write(net::buffer(msg));
                ygg::log::info("[Heimdall] HyperliquidOrderAdapter: ping sent");
            } catch (const std::exception& e) {
                ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: ping write failed: {}",
                               e.what());
                // Don't throw — let the reader detect the dead connection
                // and trigger reconnect via the normal error path.
                break;
            }
        }
    });
    // Join the ping thread on scope exit (normal exit and exceptions).
    struct JoinGuard {
        std::atomic<bool>& stop;
        std::thread& th;
        ~JoinGuard() {
            stop.store(true, std::memory_order_relaxed);
            if (th.joinable()) th.join();
        }
    } join_guard{ping_stop, ping_thread};

    beast::flat_buffer buf;
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        beast::error_code ec;
        ws->read(buf, ec);

        if (ec == beast::error::timeout) {
            buf.consume(buf.size());
            continue;
        }
        if (ec)
            throw beast::system_error(ec);

        uint64_t recv_ns = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        handle_message(std::string(static_cast<const char*>(buf.data().data()), buf.data().size()), recv_ns);
        buf.consume(buf.size());
    }

    ws->close(websocket::close_code::normal);
}

void HyperliquidOrderAdapter::send_new_order(const bifrost::protocol::NewOrder& order) {
    if (!enabled_ || !signer_) {
        ygg::log::error("[Heimdall] HyperliquidOrderAdapter: disabled, cannot send order");
        return;
    }

    using OS = bifrost::protocol::OrderSide;

    const std::string exchange_symbol = order.getExchangeSymbolAsString();
    const bool is_buy = (order.side() == OS::BUY);
    const double price_d = static_cast<double>(order.price()) / kScale;
    const double size_d  = static_cast<double>(order.quantity()) / kScale;

    try {
        // Build Hyperliquid order action JSON. The exact same JSON is
        // msgpack-encoded by the signer so the signature matches what's
        // POSTed — do NOT mutate `action` after signing.
        //
        // Hyperliquid expects "a" as the asset INDEX (integer), not a coin
        // object. The index is from /info meta.universe[].name → position.
        // For now we hardcode BTC=0 since the strategy only trades BTC; a
        // proper fix would build a coin→index map at startup from /info meta.
        // TODO: load coin→(asset_idx, szDecimals) from /info meta at startup.
        // Values below are TESTNET only and will diverge from mainnet.
        int asset_idx = 3;       // BTC on Hyperliquid testnet
        int sz_decimals = 5;     // BTC szDecimals
        if (exchange_symbol == "ETH") { asset_idx = 4; sz_decimals = 4; }
        (void)sz_decimals;
        // Price rule: max (6 - szDecimals) decimals AND max 5 significant
        // figures. For BTC (szDecimals=5) that's max 1 decimal and 5 sig
        // figs → at ~$72k, the only valid form is an integer. Round to
        // integer as the simplest compliant format for now.
        const double px_rounded = std::round(price_d);

        json::object ord;
        ord["a"] = asset_idx;
        ord["b"] = is_buy;
        ord["p"] = float_to_wire(px_rounded);
        ord["s"] = float_to_wire(size_d);
        ord["r"] = false;
        ord["t"] = json::object{{"limit", json::object{{"tif", "Gtc"}}}};

        json::object action;
        action["type"] = "order";
        action["orders"] = json::array{std::move(ord)};
        action["grouping"] = "na";

        const uint64_t nonce = signer_->next_nonce();
        auto tx = signer_->sign_l1_action(action, nonce);

        json::object signature;
        signature["r"] = "0x" + tx.r;
        signature["s"] = "0x" + tx.s;
        signature["v"] = tx.v;

        json::object req;
        req["action"] = std::move(action);
        req["nonce"] = nonce;
        req["signature"] = std::move(signature);

        std::string resp = https_post("/exchange", json::serialize(req));
        ygg::log::info("[Heimdall] HyperliquidOrderAdapter: new order id={} side={} px={} sz={} resp={}",
                       order.orderId(),
                       is_buy ? "BUY" : "SELL",
                       price_d,
                       size_d,
                       resp);

        // Parse the /exchange response and emit ACKED/FILLED/REJECTED so
        // OrderProcessor can publish ExecReports back to fenrir. Without
        // this, fenrir's strategies (e.g. Stoikov) wedge after the first
        // quote because they wait for an ack to clear in-flight state.
        //
        // Hyperliquid response shape:
        //   { "status":"ok", "response": {
        //       "type":"order",
        //       "data": { "statuses": [
        //         {"resting": {"oid": 12345}},                            // ACKED
        //         {"filled":  {"totalSz":"0.001","avgPx":"70000","oid":12345}},  // FILLED
        //         {"error":   "..."}                                       // REJECTED
        //       ] } } }
        using ES = bifrost::protocol::ExecStatus;
        using RR = bifrost::protocol::RejectReason;
        using FC = bifrost::protocol::FeeCurrency;
        try {
            auto rj = json::parse(resp).as_object();
            const std::string status = rj.contains("status") ? std::string(rj.at("status").as_string()) : "";
            const uint64_t now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            auto emit = [&](ES::Value es, RR::Value rr, uint64_t filled_qty,
                            uint64_t exch_oid) {
                ExecEvent ev{};
                ev.order_id = order.orderId();
                ev.exchange_order_id = exch_oid;
                ev.exchange_id = bifrost::protocol::ExchangeId::HYPERLIQUID;
                ev.instrument_id = order.instrumentId();
                ev.status = es;
                ev.side = order.side();
                ev.order_type = order.orderType();
                ev.price = order.price();
                ev.filled_qty = filled_qty;
                ev.remaining_qty = (order.quantity() > filled_qty) ? (order.quantity() - filled_qty) : 0;
                ev.reject_reason = rr;
                ev.fee = 0;
                ev.fee_currency = FC::USDT;
                ev.exchange_ts_ns = now_ns;
                ev.local_ts_ns = now_ns;
                if (!exec_queue_.try_push(ev))
                    ygg::log::error("[Hyperliquid] exec_queue full — dropped synthetic ExecEvent");
            };

            if (status != "ok") {
                ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: order rejected, status={}", status);
                emit(ES::REJECTED, RR::EXCHANGE_ERROR, 0, 0);
                return;
            }

            // status=ok — drill into response.data.statuses[0]
            if (!rj.contains("response")) return;
            const auto& response = rj.at("response").as_object();
            if (!response.contains("data")) return;
            const auto& data = response.at("data").as_object();
            if (!data.contains("statuses") || !data.at("statuses").is_array()) return;
            const auto& statuses = data.at("statuses").as_array();
            if (statuses.empty()) return;

            const auto& s0 = statuses[0].as_object();
            if (s0.contains("resting")) {
                // ACKED — order rests in the book.
                uint64_t exch_oid = 0;
                if (s0.at("resting").as_object().contains("oid"))
                    exch_oid = s0.at("resting").as_object().at("oid").to_number<uint64_t>();
                emit(ES::ACKED, RR::NULL_VALUE, 0, exch_oid);
            } else if (s0.contains("filled")) {
                // Immediate fill (IOC or aggressive limit).
                const auto& f = s0.at("filled").as_object();
                uint64_t exch_oid = f.contains("oid") ? f.at("oid").to_number<uint64_t>() : 0;
                double total_sz = f.contains("totalSz") ? std::stod(std::string(f.at("totalSz").as_string())) : 0.0;
                uint64_t filled_qty = static_cast<uint64_t>(std::round(total_sz * kScale));
                emit(ES::FILLED, RR::NULL_VALUE, filled_qty, exch_oid);
            } else if (s0.contains("error")) {
                ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: order error: {}",
                               std::string(s0.at("error").as_string()));
                emit(ES::REJECTED, RR::EXCHANGE_ERROR, 0, 0);
            }
        } catch (const std::exception& e) {
            ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: failed to parse order resp: {} resp={}",
                           e.what(), resp);
            // Defensive: emit a REJECTED so fenrir doesn't wedge waiting forever.
            ExecEvent ev{};
            ev.order_id = order.orderId();
            ev.exchange_id = bifrost::protocol::ExchangeId::HYPERLIQUID;
            ev.instrument_id = order.instrumentId();
            ev.status = ES::REJECTED;
            ev.side = order.side();
            ev.order_type = order.orderType();
            ev.price = order.price();
            ev.remaining_qty = order.quantity();
            ev.reject_reason = RR::EXCHANGE_ERROR;
            ev.fee_currency = FC::USDT;
            (void)exec_queue_.try_push(ev);
        }
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] HyperliquidOrderAdapter: send_new_order failed: {}", e.what());
    }
}

void HyperliquidOrderAdapter::send_cancel(const bifrost::protocol::CancelOrder& cancel,
                                          const std::string& native_symbol) {
    if (!enabled_ || !signer_) {
        ygg::log::error("[Heimdall] HyperliquidOrderAdapter: disabled, cannot cancel order");
        return;
    }

    try {
        // Build cancel action first, then sign the exact bytes we POST.
        // Hyperliquid's cancel-by-oid action shape:
        //   {"type":"cancel","cancels":[{"a":<asset_idx>,"o":<oid>}]}
        int asset_idx = 3;  // BTC on Hyperliquid testnet (see send_new_order note)
        if (native_symbol == "ETH") asset_idx = 4;

        json::object c;
        c["a"] = asset_idx;
        c["o"] = cancel.orderId();

        json::object action;
        action["type"] = "cancel";
        action["cancels"] = json::array{std::move(c)};

        const uint64_t nonce = signer_->next_nonce();
        auto tx = signer_->sign_l1_action(action, nonce);

        json::object signature;
        signature["r"] = "0x" + tx.r;
        signature["s"] = "0x" + tx.s;
        signature["v"] = tx.v;

        json::object req;
        req["action"] = std::move(action);
        req["nonce"] = nonce;
        req["signature"] = std::move(signature);

        std::string resp = https_post("/exchange", json::serialize(req));
        ygg::log::info("[Heimdall] HyperliquidOrderAdapter: cancel id={} resp={}",
                       cancel.orderId(), resp);

        // Emit a CANCELLED ExecEvent on successful cancel so OrderStateManager
        // can transition the order to a terminal state. Without this, the
        // state machine leaves the order "ACKED" forever and the stale-order
        // watchdog fires ~30s later with a synthetic CANCELLED anyway — but
        // the slot remains held in the meantime, which wedges Stoikov.
        //
        // HL cancel response shape:
        //   {"status":"ok","response":{"type":"cancel","data":{"statuses":[
        //     "success"                                   // cancelled
        //     | {"error":"Order was never placed..."}     // already gone
        //   ]}}}
        using ES = bifrost::protocol::ExecStatus;
        using RR = bifrost::protocol::RejectReason;
        using FC = bifrost::protocol::FeeCurrency;
        try {
            auto rj = json::parse(resp).as_object();
            const std::string status = rj.contains("status") ? std::string(rj.at("status").as_string()) : "";
            if (status != "ok") return;

            const uint64_t now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            ExecEvent ev{};
            ev.order_id = cancel.orderId();
            ev.exchange_id = bifrost::protocol::ExchangeId::HYPERLIQUID;
            ev.status = ES::CANCELLED;
            ev.reject_reason = RR::NULL_VALUE;
            ev.fee_currency = FC::USDT;
            ev.exchange_ts_ns = now_ns;
            ev.local_ts_ns = now_ns;
            if (!exec_queue_.try_push(ev))
                ygg::log::error("[Hyperliquid] exec_queue full — dropped CANCELLED ExecEvent");
        } catch (const std::exception& e) {
            ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: failed to parse cancel resp: {}",
                           e.what());
        }
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] HyperliquidOrderAdapter: send_cancel failed: {}", e.what());
    }
}

void HyperliquidOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: send_cancel_all instrument_id={}", instrument_id);
}

void HyperliquidOrderAdapter::send_modify(const bifrost::protocol::ModifyOrder& modify,
                                          const std::string& native_symbol) {
    if (!enabled_ || !signer_) {
        ygg::log::error("[Heimdall] HyperliquidOrderAdapter: disabled, cannot modify order");
        return;
    }

    try {
        // Build modify action first, then sign the exact bytes we POST.
        // Side is not present in ModifyOrder — we default to buy and rely
        // on upstream order-state tracking to only modify orders in-book.
        int asset_idx = 3;  // BTC on Hyperliquid testnet (see send_new_order note)
        if (native_symbol == "ETH") asset_idx = 4;

        const double price_d = static_cast<double>(modify.newPrice()) / kScale;
        const double size_d  = static_cast<double>(modify.newQuantity()) / kScale;

        json::object ord_inner;
        ord_inner["a"] = asset_idx;
        ord_inner["b"] = true;
        ord_inner["p"] = float_to_wire(std::round(price_d));
        ord_inner["s"] = float_to_wire(size_d);
        ord_inner["r"] = false;
        ord_inner["t"] = json::object{{"limit", json::object{{"tif", "Gtc"}}}};

        json::object m;
        m["oid"] = modify.orderId();
        m["order"] = std::move(ord_inner);

        json::object action;
        action["type"] = "modify";
        action["modifies"] = json::array{std::move(m)};

        const uint64_t nonce = signer_->next_nonce();
        auto tx = signer_->sign_l1_action(action, nonce);

        json::object signature;
        signature["r"] = "0x" + tx.r;
        signature["s"] = "0x" + tx.s;
        signature["v"] = tx.v;

        json::object req;
        req["action"] = std::move(action);
        req["nonce"] = nonce;
        req["signature"] = std::move(signature);

        std::string resp = https_post("/exchange", json::serialize(req));
        ygg::log::debug("[Heimdall] HyperliquidOrderAdapter: modify resp={}", resp);
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] HyperliquidOrderAdapter: send_modify failed: {}", e.what());
    }
}

AccountSnapshotData HyperliquidOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    const uint64_t ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    AccountSnapshotData snap;
    snap.exchange_id = bifrost::protocol::ExchangeId::HYPERLIQUID;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns = ts_ns;

    if (!enabled_) {
        ygg::log::warn("[Heimdall] HyperliquidOrderAdapter: disabled — returning empty snapshot");
        return snap;
    }

    // Wallet address required for clearinghouseState query.
    if (wallet_address_.empty()) {
        ygg::log::warn(
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
                snap.total_equity_e8 =
                    static_cast<int64_t>(std::round(std::stod(std::string(ms.at("accountValue").as_string())) * 1e8));
        }
        if (j.contains("withdrawable"))
            snap.available_balance_e8 =
                static_cast<int64_t>(std::round(std::stod(std::string(j.at("withdrawable").as_string())) * 1e8));

        // Positions from assetPositions.
        if (j.contains("assetPositions") && j.at("assetPositions").is_array()) {
            for (const auto& ap_val : j.at("assetPositions").as_array()) {
                if (!ap_val.is_object())
                    continue;
                const auto& ap = ap_val.as_object();
                if (!ap.contains("position") || !ap.at("position").is_object())
                    continue;
                const auto& pos = ap.at("position").as_object();
                if (!pos.contains("szi"))
                    continue;
                const double szi = std::stod(std::string(pos.at("szi").as_string()));
                if (szi == 0.0)
                    continue;

                AccountPosition p;
                if (pos.contains("coin"))
                    p.exchange_symbol = std::string(pos.at("coin").as_string());
                p.net_qty_e8 = static_cast<int64_t>(std::round(szi * 1e8));
                if (pos.contains("entryPx") && pos.at("entryPx").is_string())
                    p.avg_entry_price_e8 =
                        static_cast<int64_t>(std::round(std::stod(std::string(pos.at("entryPx").as_string())) * 1e8));
                if (pos.contains("unrealizedPnl") && pos.at("unrealizedPnl").is_string())
                    p.unrealized_pnl_e8 = static_cast<int64_t>(
                        std::round(std::stod(std::string(pos.at("unrealizedPnl").as_string())) * 1e8));
                snap.positions.push_back(std::move(p));
            }
        }
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] HyperliquidOrderAdapter: failed to parse account snapshot: {}", e.what());
    }

    ygg::log::info(
        "[Heimdall] HyperliquidOrderAdapter: account snapshot fetched — balance={:.2f} "
        "positions={}",
        static_cast<double>(snap.available_balance_e8) / 1e8,
        snap.positions.size());
    return snap;
}

}  // namespace heimdall::adapter
