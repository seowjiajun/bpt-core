#include "order_gateway/adapter/okx/okx_order_adapter.h"

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/okx/okx_action_encoder.h"
#include "order_gateway/adapter/okx/okx_auth.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <chrono>
#include <string>

namespace bpt::order_gateway::adapter {

namespace json = boost::json;

OKXOrderAdapter::OKXOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg),
      api_key_(creds.api_key),
      secret_key_(creds.secret_key),
      passphrase_(creds.passphrase),
      https_client_(cfg_, creds),
      instruments_(https_client_),
      ws_client_(ioc_, ssl_ctx_, cfg_) {
    uint32_t epoch_s = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", epoch_s);
    session_prefix_ = std::string(buf, 8);

    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            bpt::common::log::error("[OKX] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };

    ws_client_.set_login_msg_builder(
        [this] { return okx::build_login_msg(api_key_, secret_key_, passphrase_); });
    ws_client_.set_message_handler(
        [this](const std::string& payload, uint64_t recv_ns) { handle_message(payload, recv_ns); });
}

void OKXOrderAdapter::start() {
    // Instruments are only available from the real OKX REST API — skip in backtest
    // (use_tls=false means we're talking to a local simulation server).
    if (cfg_.use_tls) {
        instruments_.fetch();
        parser_.set_contract_sizes(instruments_.contract_sizes());
        fetch_and_log_account_config();
    }
    OrderAdapterBase::start();
}

void OKXOrderAdapter::handle_message(const std::string& payload, uint64_t recv_ns) {
    // NOTE: do NOT log the raw payload here. OKX login frames include
    // the apiKey + passphrase + HMAC sign fields in plaintext JSON; the
    // login response can echo parts of the request on error. The
    // downstream parsing below emits structured logs (event type, code,
    // msg, order IDs) that are the right diagnostic surface. If you need
    // raw-payload visibility for debugging, gate it behind a build flag
    // or a dev-only debug channel — never an INFO log in the hot path.
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
            bpt::common::log::error("OKXOrderAdapter: error event code={} msg={}", code, msg);
        } else {
            bpt::common::log::info("OKXOrderAdapter: event={}", event);
        }
        if (event == "login") {
            bpt::common::log::info("OKXOrderAdapter: login successful");
            logged_in_.store(true, std::memory_order_release);
            json::object sub_msg;
            sub_msg["op"] = "subscribe";
            json::array args;
            json::object arg;
            arg["channel"] = "orders";
            arg["instType"] = "ANY";
            args.push_back(arg);
            sub_msg["args"] = std::move(args);
            ws_client_.send(json::serialize(sub_msg));
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
    ws_client_.run(stop_flag_, connected_);
}

void OKXOrderAdapter::send_new_order(const bpt::messages::NewOrder& order) {
    const std::string exchange_symbol = order.getExchangeSymbolAsString();
    const std::string cloid = session_prefix_ + "G" + std::to_string(order.orderId());
    parser_.register_order(cloid, order.orderId());

    const okx::OrderSpec spec{
        exchange_symbol,
        order.side(),
        order.orderType(),
        order.timeInForce(),
        order.price(),
        order.quantity(),
        cloid,
    };
    const uint64_t req_id = ws_req_id_.fetch_add(1, std::memory_order_relaxed);
    const std::string frame = json::serialize(
        okx::build_order_action(spec, req_id, instruments_.inst_id_codes(), instruments_.contract_sizes()));
    auto emit_rejection = [&]() {
        uint64_t ts = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        ExecEvent rej;
        rej.order_id = order.orderId();
        rej.exchange_id = bpt::messages::ExchangeId::OKX;
        rej.instrument_id = 0;
        rej.local_ts_ns = ts;
        rej.exchange_ts_ns = ts;
        rej.side = (order.side() == bpt::messages::OrderSide::BUY) ? bpt::messages::OrderSide::BUY
                                                                       : bpt::messages::OrderSide::SELL;
        rej.order_type = order.orderType();
        rej.status = bpt::messages::ExecStatus::REJECTED;
        rej.reject_reason = bpt::messages::RejectReason::EXCHANGE_ERROR;
        if (!exec_queue_.try_push(rej))
            bpt::common::log::error("[OKX] exec_queue full — dropped rejection order_id={}", rej.order_id);
    };

    try {
        if (!ws_client_.send(frame)) {
            bpt::common::log::warn(
                "OKXOrderAdapter: send_new_order: WS not connected, "
                "rejecting order={}",
                order.orderId());
            emit_rejection();
        }
    } catch (const std::exception& e) {
        bpt::common::log::error("OKXOrderAdapter: send_new_order failed: {}", e.what());
        emit_rejection();
    }
}

void OKXOrderAdapter::send_cancel(const bpt::messages::CancelOrder& cancel, const std::string& native_symbol) {
    const std::string cloid = session_prefix_ + "G" + std::to_string(cancel.orderId());
    const uint64_t req_id = ws_req_id_.fetch_add(1, std::memory_order_relaxed);
    const std::string frame = json::serialize(
        okx::build_cancel_action(native_symbol, cloid, req_id));

    try {
        ws_client_.send(frame);
    } catch (const std::exception& e) {
        bpt::common::log::error("OKXOrderAdapter: send_cancel failed: {}", e.what());
    }
}

void OKXOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    bpt::common::log::warn("OKXOrderAdapter: send_cancel_all called instrument_id={}", instrument_id);
}

void OKXOrderAdapter::send_modify(const bpt::messages::ModifyOrder& modify, const std::string& native_symbol) {
    const std::string cloid = session_prefix_ + "G" + std::to_string(modify.orderId());
    const std::string frame = json::serialize(
        okx::build_modify_action(native_symbol, cloid, modify.newPrice(), modify.newQuantity(),
                                 instruments_.contract_sizes()));

    try {
        ws_client_.send(frame);
    } catch (const std::exception& e) {
        bpt::common::log::error("OKXOrderAdapter: send_modify failed: {}", e.what());
    }
}

AccountSnapshotData OKXOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    const uint64_t ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    AccountSnapshotData snap;
    snap.exchange_id = bpt::messages::ExchangeId::OKX;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns = ts_ns;

    try {
        // Balance endpoint — totalEq and per-ccy availBal/eq details.
        auto bal_resp = https_client_.get_signed("/api/v5/account/balance");
        auto bal_j = json::parse(bal_resp);
        if (!bal_j.is_object()) {
            // Non-JSON response (WAF interstitial, gateway error page). Log
            // size only — a byte count is enough to distinguish HTML blob
            // from empty body without echoing whatever the origin returned.
            bpt::common::log::warn("OKXOrderAdapter: balance response is not a JSON object (bytes={})",
                           bal_resp.size());
        } else {
            const auto& root = bal_j.as_object();
            // OKX wraps errors in {code, msg, data:[]}. Surface any non-zero
            // code prominently so we don't silently swallow auth / clock
            // failures like 50112. The structured code+msg pair is the
            // entire diagnostic surface; dropping the raw body avoids
            // echoing signed-request metadata or upstream error envelopes.
            if (root.contains("code") && std::string(root.at("code").as_string()) != "0") {
                bpt::common::log::warn("OKXOrderAdapter: /account/balance error code={} msg={}",
                               std::string(root.at("code").as_string()),
                               root.contains("msg") ? std::string(root.at("msg").as_string()) : "");
            } else if (root.contains("data") && root.at("data").is_array() &&
                       !root.at("data").as_array().empty() && root.at("data").as_array()[0].is_object()) {
                const auto& d = root.at("data").as_array()[0].as_object();
                if (d.contains("totalEq"))
                    snap.total_equity_e8 =
                        static_cast<int64_t>(std::round(std::stod(std::string(d.at("totalEq").as_string())) * 1e8));
                // Per-ccy detail scan: record non-zero currency balances in
                // snap.currency_balances (one row per ccy for the dashboard)
                // AND capture the USDT availBal for snap.available_balance_e8.
                if (d.contains("details") && d.at("details").is_array()) {
                    for (const auto& detail : d.at("details").as_array()) {
                        if (!detail.is_object())
                            continue;
                        const auto& de = detail.as_object();
                        const std::string ccy = de.contains("ccy") ? std::string(de.at("ccy").as_string()) : "";
                        const std::string eq = de.contains("eq") ? std::string(de.at("eq").as_string()) : "0";
                        const std::string avail =
                            de.contains("availBal") ? std::string(de.at("availBal").as_string()) : "0";
                        const double eq_d = eq.empty() ? 0.0 : std::stod(eq);
                        const double avail_d = avail.empty() ? 0.0 : std::stod(avail);
                        if (eq_d > 0.0)
                            bpt::common::log::info("[OKX balance detail] ccy={} eq={} availBal={}", ccy, eq, avail);
                        // Cap ccy name to 8 chars — SBE group field is Char8.
                        // Any OKX ccy code in the wild is ≤ 5 chars so this
                        // is a safety clamp, not a real concern.
                        if (eq_d > 0.0 && !ccy.empty()) {
                            snap.currency_balances.push_back({
                                ccy.substr(0, 8),
                                static_cast<int64_t>(std::round(eq_d * 1e8)),
                                static_cast<int64_t>(std::round(avail_d * 1e8)),
                            });
                        }
                        if (ccy == "USDT" && de.contains("availBal"))
                            snap.available_balance_e8 =
                                static_cast<int64_t>(std::round(avail_d * 1e8));
                    }
                }
                bpt::common::log::info("OKXOrderAdapter: /account/balance totalEq={:.4f} USDT availBal={:.4f}",
                               static_cast<double>(snap.total_equity_e8) / 1e8,
                               static_cast<double>(snap.available_balance_e8) / 1e8);
            } else {
                bpt::common::log::warn("OKXOrderAdapter: balance response missing data[]: {}",
                               bal_resp.substr(0, 400));
            }
        }
    } catch (const std::exception& e) {
        bpt::common::log::warn("OKXOrderAdapter: failed to fetch balance: {}", e.what());
    }

    try {
        // Positions endpoint.
        auto pos_resp = https_client_.get_signed("/api/v5/account/positions");
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
        bpt::common::log::warn("OKXOrderAdapter: failed to fetch positions: {}", e.what());
    }

    bpt::common::log::info("OKXOrderAdapter: account snapshot fetched — balance={:.2f} positions={}",
                   static_cast<double>(snap.available_balance_e8) / 1e8,
                   snap.positions.size());
    return snap;
}

void OKXOrderAdapter::fetch_and_log_account_config() {
    try {
        auto body = https_client_.get_signed("/api/v5/account/config");
        auto j = json::parse(body);
        if (!j.is_object()) {
            bpt::common::log::warn("[OKX account/config] unexpected response shape");
            return;
        }
        auto& obj = j.as_object();
        if (obj.contains("code") && obj.at("code").as_string() != "0") {
            bpt::common::log::warn("[OKX account/config] error code={} msg={}",
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

        bpt::common::log::info("[OKX account/config] uid={} label='{}' acctLv={} ({}) perm={} posMode={}",
                       uid, label, acct_lv, lvl_name, perm, pos_mode);

        if (!derivatives_allowed) {
            bpt::common::log::warn("[OKX account/config] ACCOUNT CANNOT TRADE DERIVATIVES — "
                           "level {} is spot-only. Perp/futures/options orders will reject "
                           "with sCode=51155 'local compliance requirements' (the real cause "
                           "is the account level, not geo-compliance). Upgrade via OKX UI: "
                           "Demo Trading → Account Mode → Single-currency margin or higher.",
                           acct_lv);
        }
    } catch (const std::exception& e) {
        bpt::common::log::warn("[OKX account/config] fetch failed: {}", e.what());
    }
}

}  // namespace bpt::order_gateway::adapter
