#include "order_gateway/adapter/deribit/deribit_order_adapter.h"

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/deribit/deribit_action_codec.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <chrono>
#include <string>
#include <yggdrasil/util/tsc_clock.h>

namespace bpt::order_gateway::adapter {

namespace json = boost::json;

DeribitOrderAdapter::DeribitOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg),
      client_id_(creds.client_id),
      client_secret_(creds.client_secret),
      ws_client_(ioc_, ssl_ctx_, cfg_) {
    uint32_t epoch_s = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", epoch_s);
    session_prefix_ = std::string(buf, 8);

    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            ygg::log::error("[Deribit] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };

    ws_client_.set_login_msg_builder([this] {
        return deribit::build_auth_msg(client_id_, client_secret_,
                                        jsonrpc_id_.fetch_add(1, std::memory_order_relaxed));
    });
    ws_client_.set_message_handler(
        [this](const std::string& payload, uint64_t recv_ns) { handle_message(payload, recv_ns); });
}

void DeribitOrderAdapter::handle_message(const std::string& payload, uint64_t recv_ns) {
    ygg::log::info("[Heimdall] DeribitOrderAdapter WS rx: {}", payload.substr(0, 500));
    auto root = json::parse(payload);
    if (!root.is_object())
        return;
    const auto& obj = root.as_object();

    // Notification methods: heartbeat + subscription channel pushes
    if (auto method_it = obj.find("method"); method_it != obj.end()) {
        const std::string method = std::string(method_it->value().as_string());

        if (method == "heartbeat") {
            auto params_it = obj.find("params");
            if (params_it != obj.end() && params_it->value().is_object()) {
                auto type_it = params_it->value().as_object().find("type");
                if (type_it != params_it->value().as_object().end() &&
                    std::string(type_it->value().as_string()) == "test_request") {
                    ws_client_.send(deribit::build_test_response(
                        jsonrpc_id_.fetch_add(1, std::memory_order_relaxed)));
                }
            }
            return;
        }

        if (method == "subscription") {
            auto params_it = obj.find("params");
            if (params_it == obj.end() || !params_it->value().is_object())
                return;
            const auto& params = params_it->value().as_object();
            auto channel_it = params.find("channel");
            if (channel_it == params.end())
                return;
            if (std::string(channel_it->value().as_string()) == "user.orders.any.raw") {
                auto data_it = params.find("data");
                if (data_it != params.end() && data_it->value().is_object())
                    parser_.handle_subscription_event(data_it->value().as_object(), recv_ns);
            }
        }
        return;
    }

    // JSON-RPC responses (id-based)
    if (obj.find("id") == obj.end())
        return;

    if (auto err_it = obj.find("error"); err_it != obj.end()) {
        const auto& err = err_it->value().as_object();
        int64_t code = 0;
        std::string errmsg;
        if (auto cit = err.find("code"); cit != err.end())
            code = cit->value().to_number<int64_t>();
        if (auto mit = err.find("message"); mit != err.end())
            errmsg = std::string(mit->value().as_string());
        ygg::log::error("[Heimdall] DeribitOrderAdapter: JSON-RPC error code={} msg={}", code, errmsg);
        return;
    }

    auto result_it = obj.find("result");
    if (result_it == obj.end() || !result_it->value().is_object())
        return;
    const auto& res = result_it->value().as_object();

    // Auth response
    if (res.find("access_token") != res.end()) {
        ygg::log::info("[Heimdall] DeribitOrderAdapter: authenticated successfully");
        logged_in_.store(true, std::memory_order_release);

        const auto next_id = [this] { return jsonrpc_id_.fetch_add(1, std::memory_order_relaxed); };
        ws_client_.send(deribit::build_simple_rpc("private/enable_cancel_on_disconnect", "", next_id()));
        ws_client_.send(deribit::build_simple_rpc("public/set_heartbeat", "{\"interval\":10}", next_id()));
        ws_client_.send(deribit::build_simple_rpc(
            "private/subscribe", "{\"channels\":[\"user.orders.any.raw\"]}", next_id()));

        std::lock_guard<std::mutex> lk(pending_mu_);
        for (const auto& frame : pending_sends_)
            ws_client_.send(frame);
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
    ws_client_.run(stop_flag_, connected_);
}

void DeribitOrderAdapter::send_new_order(const bpt::messages::NewOrder& order) {
    const std::string exchange_symbol = order.getExchangeSymbolAsString();
    const std::string label = session_prefix_ + "G" + std::to_string(order.orderId());
    parser_.register_order(label, order.orderId());

    const deribit::OrderSpec spec{
        exchange_symbol,
        order.side(),
        order.orderType(),
        order.timeInForce(),
        order.price(),
        order.quantity(),
        label,
    };
    const std::string frame =
        deribit::build_new_order_msg(spec, jsonrpc_id_.fetch_add(1, std::memory_order_relaxed));

    auto emit_rejection = [&]() {
        const uint64_t ts = ygg::util::WallClock::now_ns();
        ExecEvent rej{};
        rej.order_id = order.orderId();
        rej.exchange_id = bpt::messages::ExchangeId::DERIBIT;
        rej.instrument_id = 0;
        rej.local_ts_ns = ts;
        rej.exchange_ts_ns = ts;
        rej.side = order.side();
        rej.order_type = order.orderType();
        rej.status = bpt::messages::ExecStatus::REJECTED;
        rej.reject_reason = bpt::messages::RejectReason::EXCHANGE_ERROR;
        if (!exec_queue_.try_push(rej))
            ygg::log::error("[Deribit] exec_queue full — dropped rejection order_id={}", rej.order_id);
    };

    if (!logged_in_.load(std::memory_order_acquire)) {
        ygg::log::info(
            "[Heimdall] DeribitOrderAdapter: queuing order {} (not yet "
            "authenticated)",
            order.orderId());
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_sends_.push_back(frame);
        return;
    }

    try {
        if (!ws_client_.send(frame)) {
            ygg::log::warn(
                "[Heimdall] DeribitOrderAdapter: send_new_order: WS not "
                "connected, rejecting order={}",
                order.orderId());
            emit_rejection();
        }
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] DeribitOrderAdapter: send_new_order failed: {}", e.what());
        emit_rejection();
    }
}

void DeribitOrderAdapter::send_cancel(const bpt::messages::CancelOrder& cancel,
                                      const std::string& /*native_symbol*/) {
    // Deribit cancel uses exchange order_id, not instrument symbol.
    const std::string exch_oid = parser_.get_exchange_order_id(cancel.orderId());
    if (exch_oid.empty()) {
        ygg::log::warn(
            "[Heimdall] DeribitOrderAdapter: send_cancel: no exchange "
            "order_id for order={}",
            cancel.orderId());
        return;
    }

    const std::string frame =
        deribit::build_cancel_msg(exch_oid, jsonrpc_id_.fetch_add(1, std::memory_order_relaxed));
    try {
        ws_client_.send(frame);
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] DeribitOrderAdapter: send_cancel failed: {}", e.what());
    }
}

void DeribitOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    ygg::log::warn(
        "[Heimdall] DeribitOrderAdapter: send_cancel_all called "
        "instrument_id={} — not supported without instrument name",
        instrument_id);
}

void DeribitOrderAdapter::send_modify(const bpt::messages::ModifyOrder& modify,
                                      const std::string& /*native_symbol*/) {
    const std::string exch_oid = parser_.get_exchange_order_id(modify.orderId());
    if (exch_oid.empty()) {
        ygg::log::warn(
            "[Heimdall] DeribitOrderAdapter: send_modify: no exchange "
            "order_id for order={}",
            modify.orderId());
        return;
    }

    const std::string frame = deribit::build_edit_msg(exch_oid, modify.newPrice(), modify.newQuantity(),
                                                        jsonrpc_id_.fetch_add(1, std::memory_order_relaxed));
    try {
        ws_client_.send(frame);
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] DeribitOrderAdapter: send_modify failed: {}", e.what());
    }
}

AccountSnapshotData DeribitOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    ygg::log::warn(
        "[Heimdall] DeribitOrderAdapter: fetch_account_snapshot not implemented — returning empty "
        "snapshot");
    AccountSnapshotData snap;
    snap.exchange_id = bpt::messages::ExchangeId::DERIBIT;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns = ygg::util::WallClock::now_ns();
    return snap;
}

}  // namespace bpt::order_gateway::adapter
