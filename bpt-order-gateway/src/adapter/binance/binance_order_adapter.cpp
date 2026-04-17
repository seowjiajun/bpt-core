#include "order_gateway/adapter/binance/binance_order_adapter.h"

#include "order_gateway/adapter/binance/binance_action_codec.h"
#include "order_gateway/adapter/binance/binance_auth.h"
#include "order_gateway/adapter/common/credentials.h"

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

BinanceOrderAdapter::BinanceOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg),
      api_key_(creds.api_key),
      secret_key_(creds.secret_key),
      https_client_(cfg_, creds),
      user_data_ws_(ioc_, ssl_ctx_, cfg_, https_client_) {
    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            ygg::log::error("[Binance] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };
    user_data_ws_.set_message_handler(
        [this](const std::string& payload, uint64_t recv_ns) { handle_user_data_message(payload, recv_ns); });
}

void BinanceOrderAdapter::handle_user_data_message(const std::string& payload, uint64_t recv_ns) {
    auto root = json::parse(payload);
    if (!root.is_object())
        return;
    parser_.handle_execution_report(root.as_object(), recv_ns);
}

void BinanceOrderAdapter::connect_and_run() {
    user_data_ws_.run(stop_flag_, connected_);
}

void BinanceOrderAdapter::send_new_order(const bpt::messages::NewOrder& order) {
    const std::string exchange_symbol = order.getExchangeSymbolAsString();
    const std::string cloid = "G" + std::to_string(order.orderId());
    parser_.register_order(cloid, order.orderId());

    const binance::OrderSpec spec{
        exchange_symbol,
        order.side(),
        order.orderType(),
        order.timeInForce(),
        order.price(),
        order.quantity(),
        cloid,
    };
    const std::string params = binance::build_new_order_params(spec);
    const std::string signed_params = binance::sign_query(secret_key_, params);

    auto emit_rejection = [&]() {
        const uint64_t ts = ygg::util::WallClock::now_ns();
        ExecEvent rej;
        rej.order_id = order.orderId();
        rej.exchange_id = bpt::messages::ExchangeId::BINANCE;
        rej.instrument_id = 0;
        rej.local_ts_ns = ts;
        rej.exchange_ts_ns = ts;
        rej.side = order.side();
        rej.order_type = order.orderType();
        rej.status = bpt::messages::ExecStatus::REJECTED;
        rej.reject_reason = bpt::messages::RejectReason::EXCHANGE_ERROR;
        if (!exec_queue_.try_push(rej))
            ygg::log::error("[Binance] exec_queue full — dropped rejection order_id={}", rej.order_id);
    };

    try {
        const std::string resp = https_client_.request("POST", "/api/v3/order?" + signed_params, "", true);
        ygg::log::debug("[Heimdall] BinanceOrderAdapter: new order resp={}", resp);

        const uint64_t recv_ns = ygg::util::WallClock::now_ns();
        auto root = json::parse(resp);
        if (!root.is_object())
            return;
        parser_.handle_order_response(root.as_object(), order.orderId(), order.side(), order.orderType(), recv_ns);
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] BinanceOrderAdapter: send_new_order failed: {}", e.what());
        emit_rejection();
    }
}

void BinanceOrderAdapter::send_cancel(const bpt::messages::CancelOrder& cancel, const std::string& native_symbol) {
    const std::string cloid = "G" + std::to_string(cancel.orderId());
    const std::string params = binance::build_cancel_params(native_symbol, cloid);
    const std::string signed_params = binance::sign_query(secret_key_, params);

    try {
        const std::string resp = https_client_.request("DELETE", "/api/v3/order?" + signed_params, "", true);
        ygg::log::debug("[Heimdall] BinanceOrderAdapter: cancel resp={}", resp);
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] BinanceOrderAdapter: send_cancel failed: {}", e.what());
    }
}

void BinanceOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    (void)instrument_id;
    ygg::log::warn(
        "[Heimdall] BinanceOrderAdapter: send_cancel_all called with "
        "instrument_id={}",
        instrument_id);
}

void BinanceOrderAdapter::send_modify(const bpt::messages::ModifyOrder& modify, const std::string& native_symbol) {
    // Binance has no native amend — cancel + replace.
    const std::string cloid = "G" + std::to_string(modify.orderId());
    const std::string new_cloid = cloid + "m";

    const std::string cancel_params = binance::build_cancel_params(native_symbol, cloid);
    const std::string signed_cancel = binance::sign_query(secret_key_, cancel_params);

    const std::string new_params =
        binance::build_modify_replace_params(native_symbol, new_cloid, modify.newPrice(), modify.newQuantity());
    const std::string signed_new = binance::sign_query(secret_key_, new_params);

    try {
        https_client_.request("DELETE", "/api/v3/order?" + signed_cancel, "", true);
        https_client_.request("POST", "/api/v3/order?" + signed_new, "", true);
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] BinanceOrderAdapter: send_modify failed: {}", e.what());
    }
}

AccountSnapshotData BinanceOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    using namespace std::chrono;
    const uint64_t ts_ns = ygg::util::WallClock::now_ns();
    const uint64_t ts_ms =
        static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());

    // GET /fapi/v2/account — futures/perp account balance and open positions.
    const std::string params = "timestamp=" + std::to_string(ts_ms);
    const std::string signed_params = binance::sign_query(secret_key_, params);
    const std::string resp = https_client_.request("GET", "/fapi/v2/account?" + signed_params, "", true);

    AccountSnapshotData snap;
    snap.exchange_id = bpt::messages::ExchangeId::BINANCE;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns = ts_ns;

    try {
        auto root = json::parse(resp);
        if (!root.is_object())
            return snap;
        const auto& obj = root.as_object();

        if (obj.contains("availableBalance"))
            snap.available_balance_e8 =
                static_cast<int64_t>(std::round(std::stod(std::string(obj.at("availableBalance").as_string())) * 1e8));
        if (obj.contains("totalWalletBalance"))
            snap.total_equity_e8 = static_cast<int64_t>(
                std::round(std::stod(std::string(obj.at("totalWalletBalance").as_string())) * 1e8));
        if (obj.contains("totalUnrealizedProfit"))
            snap.total_equity_e8 += static_cast<int64_t>(
                std::round(std::stod(std::string(obj.at("totalUnrealizedProfit").as_string())) * 1e8));

        if (obj.contains("positions") && obj.at("positions").is_array()) {
            for (const auto& p : obj.at("positions").as_array()) {
                if (!p.is_object())
                    continue;
                const auto& po = p.as_object();
                const double pos_amt = std::stod(std::string(po.at("positionAmt").as_string()));
                if (pos_amt == 0.0)
                    continue;

                AccountPosition ap;
                ap.exchange_symbol = std::string(po.at("symbol").as_string());
                ap.net_qty_e8 = static_cast<int64_t>(std::round(pos_amt * 1e8));
                if (po.contains("entryPrice"))
                    ap.avg_entry_price_e8 =
                        static_cast<int64_t>(std::round(std::stod(std::string(po.at("entryPrice").as_string())) * 1e8));
                if (po.contains("unrealizedProfit"))
                    ap.unrealized_pnl_e8 = static_cast<int64_t>(
                        std::round(std::stod(std::string(po.at("unrealizedProfit").as_string())) * 1e8));
                snap.positions.push_back(std::move(ap));
            }
        }
    } catch (const std::exception& e) {
        ygg::log::error("[Heimdall] BinanceOrderAdapter: failed to parse account snapshot: {}", e.what());
    }

    ygg::log::info("[Heimdall] BinanceOrderAdapter: account snapshot fetched — balance={:.2f} positions={}",
                   static_cast<double>(snap.available_balance_e8) / 1e8,
                   snap.positions.size());
    return snap;
}

}  // namespace bpt::order_gateway::adapter
