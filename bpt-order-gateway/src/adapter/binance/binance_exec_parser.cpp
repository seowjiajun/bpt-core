#include "order_gateway/adapter/binance/binance_exec_parser.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <string>
#include <bpt_common/logging.h>

namespace bpt::order_gateway::adapter {

namespace json = boost::json;

static constexpr double kScale = 1e8;

static bpt::messages::FeeCurrency::Value parse_fee_currency(const std::string& asset) {
    using FC = bpt::messages::FeeCurrency;
    if (asset == "BTC")
        return FC::BTC;
    if (asset == "ETH")
        return FC::ETH;
    if (asset == "BNB")
        return FC::BNB;
    if (asset == "USDT")
        return FC::USDT;
    return FC::USDT;
}

void BinanceExecParser::register_order(const std::string& cloid, uint64_t order_id) {
    std::lock_guard<std::mutex> lk(mu_);
    cloid_to_order_id_[cloid] = order_id;
}

void BinanceExecParser::handle_execution_report(const json::object& obj, uint64_t recv_ns) {
    auto eit = obj.find("e");
    if (eit == obj.end())
        return;
    if (std::string(eit->value().as_string()) != "executionReport")
        return;

    std::string exec_type = std::string(obj.at("X").as_string());
    std::string cloid = std::string(obj.at("c").as_string());

    uint64_t order_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cloid_to_order_id_.find(cloid);
        if (it != cloid_to_order_id_.end())
            order_id = it->second;
    }
    if (order_id == 0) {
        bpt::common::log::warn("BinanceExecParser: unknown cloid={}", cloid);
        return;
    }

    using ES = bpt::messages::ExecStatus;
    using OS = bpt::messages::OrderSide;
    using OT = bpt::messages::OrderType;
    using RR = bpt::messages::RejectReason;

    ExecEvent ev{};
    ev.order_id = order_id;
    ev.exchange_order_id = static_cast<uint64_t>(obj.at("i").as_int64());
    ev.exchange_id = bpt::messages::ExchangeId::BINANCE;
    ev.instrument_id = 0;
    ev.local_ts_ns = recv_ns;

    std::string side_str = std::string(obj.at("S").as_string());
    ev.side = (side_str == "BUY") ? OS::BUY : OS::SELL;

    std::string type_str = std::string(obj.at("o").as_string());
    if (type_str == "MARKET")
        ev.order_type = OT::MARKET;
    else if (type_str == "LIMIT_MAKER")
        ev.order_type = OT::POST_ONLY;
    else
        ev.order_type = OT::LIMIT;

    ev.price = static_cast<int64_t>(std::stod(std::string(obj.at("p").as_string())) * kScale);
    ev.filled_qty = static_cast<uint64_t>(std::stod(std::string(obj.at("z").as_string())) * kScale);
    uint64_t total_qty = static_cast<uint64_t>(std::stod(std::string(obj.at("q").as_string())) * kScale);
    ev.remaining_qty = total_qty > ev.filled_qty ? total_qty - ev.filled_qty : 0;

    ev.fee = static_cast<int64_t>(std::stod(std::string(obj.at("n").as_string())) * kScale);
    ev.fee_currency = parse_fee_currency(std::string(obj.at("N").as_string()));

    ev.exchange_ts_ns = static_cast<uint64_t>(obj.at("T").as_int64()) * 1000000ULL;
    ev.reject_reason = RR::OK;

    if (exec_type == "NEW") {
        ev.status = ES::ACKED;
    } else if (exec_type == "TRADE") {
        ev.status = (ev.remaining_qty == 0) ? ES::FILLED : ES::PARTIAL;
    } else if (exec_type == "CANCELED" || exec_type == "EXPIRED") {
        ev.status = ES::CANCELLED;
    } else if (exec_type == "REJECTED") {
        ev.status = ES::REJECTED;
        ev.reject_reason = RR::EXCHANGE_ERROR;
    } else {
        return;  // ignore PENDING_CANCEL and other types
    }

    if (on_exec_event)
        on_exec_event(ev);
}

void BinanceExecParser::handle_order_response(const json::object& obj,
                                               uint64_t order_id,
                                               bpt::messages::OrderSide::Value side,
                                               bpt::messages::OrderType::Value order_type,
                                               uint64_t recv_ns) {
    using ES = bpt::messages::ExecStatus;
    using RR = bpt::messages::RejectReason;

    ExecEvent ev{};
    ev.order_id = order_id;
    ev.exchange_id = bpt::messages::ExchangeId::BINANCE;
    ev.instrument_id = 0;
    ev.local_ts_ns = recv_ns;
    ev.reject_reason = RR::OK;
    ev.side = side;
    ev.order_type = order_type;

    // Binance error response has "code" field (negative integer).
    if (auto code_it = obj.find("code"); code_it != obj.end()) {
        auto msg_it = obj.find("msg");
        std::string msg = (msg_it != obj.end()) ? std::string(msg_it->value().as_string()) : "?";
        bpt::common::log::error(
            "BinanceExecParser: exchange rejected order={} code={} msg={}",
            order_id,
            code_it->value().as_int64(),
            msg);
        ev.exchange_ts_ns = recv_ns;
        ev.status = ES::REJECTED;
        ev.reject_reason = RR::EXCHANGE_ERROR;
        if (on_exec_event)
            on_exec_event(ev);
        return;
    }

    if (auto oit = obj.find("orderId"); oit != obj.end())
        ev.exchange_order_id = static_cast<uint64_t>(oit->value().as_int64());

    ev.price = static_cast<int64_t>(std::stod(std::string(obj.at("price").as_string())) * kScale);
    ev.filled_qty = static_cast<uint64_t>(std::stod(std::string(obj.at("executedQty").as_string())) * kScale);
    const uint64_t total_qty = static_cast<uint64_t>(std::stod(std::string(obj.at("origQty").as_string())) * kScale);
    ev.remaining_qty = total_qty > ev.filled_qty ? total_qty - ev.filled_qty : 0;

    if (auto tsit = obj.find("transactTime"); tsit != obj.end())
        ev.exchange_ts_ns = static_cast<uint64_t>(tsit->value().as_int64()) * 1000000ULL;
    else
        ev.exchange_ts_ns = recv_ns;

    // Fee: sum fills if present.
    ev.fee = 0;
    ev.fee_currency = bpt::messages::FeeCurrency::USDT;
    if (auto fit = obj.find("fills"); fit != obj.end() && fit->value().is_array()) {
        for (const auto& fill : fit->value().as_array()) {
            const auto& f = fill.as_object();
            ev.fee += static_cast<int64_t>(std::stod(std::string(f.at("commission").as_string())) * kScale);
            if (auto fcit = f.find("commissionAsset"); fcit != f.end())
                ev.fee_currency = parse_fee_currency(std::string(fcit->value().as_string()));
        }
    }

    const std::string status = std::string(obj.at("status").as_string());
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

    if (on_exec_event)
        on_exec_event(ev);
}

}  // namespace bpt::order_gateway::adapter
