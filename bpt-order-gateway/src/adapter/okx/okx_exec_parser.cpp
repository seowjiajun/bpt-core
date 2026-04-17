#include "order_gateway/adapter/okx/okx_exec_parser.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <cmath>
#include <string>
#include <yggdrasil/logging.h>

namespace bpt::order_gateway::adapter {

namespace json = boost::json;

static constexpr double kPriceScale = 1e8;
// Wire qty scale is 1e8 across all order-gateway adapters — matches Binance,
// Hyperliquid, Deribit, and the SBE protocol. Was 1e5 before the qty-
// scale fix; see okx_action_codec.cpp for the bug write-up.
static constexpr double kQtyScale = 1e8;

static bpt::messages::FeeCurrency::Value parse_fee_ccy(const std::string& ccy) {
    using FC = bpt::messages::FeeCurrency;
    if (ccy == "BTC")
        return FC::BTC;
    if (ccy == "ETH")
        return FC::ETH;
    if (ccy == "USDT")
        return FC::USDT;
    if (ccy == "USD")
        return FC::USD;
    return FC::USDT;
}

void OKXExecParser::register_order(const std::string& cloid, uint64_t order_id) {
    std::lock_guard<std::mutex> lk(mu_);
    cloid_to_order_id_[cloid] = order_id;
}

void OKXExecParser::set_contract_sizes(const std::unordered_map<std::string, double>& sizes) {
    std::lock_guard<std::mutex> lk(mu_);
    contract_sizes_ = sizes;
}

void OKXExecParser::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    acked_orders_.clear();
    cancelled_orders_.clear();
}

void OKXExecParser::handle_order_ack(const json::object& d, uint64_t recv_ns) {
    using ES = bpt::messages::ExecStatus;
    using RR = bpt::messages::RejectReason;

    std::string cloid;
    if (auto cit = d.find("clOrdId"); cit != d.end())
        cloid = std::string(cit->value().as_string());

    uint64_t order_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cloid_to_order_id_.find(cloid);
        if (it != cloid_to_order_id_.end())
            order_id = it->second;
    }
    if (order_id == 0) {
        ygg::log::warn("[Heimdall] OKXExecParser: op=order response unknown cloid={}", cloid);
        return;
    }

    std::string s_code;
    if (auto sc = d.find("sCode"); sc != d.end())
        s_code = std::string(sc->value().as_string());
    std::string s_msg;
    if (auto sm = d.find("sMsg"); sm != d.end())
        s_msg = std::string(sm->value().as_string());

    if (s_code == "0") {
        uint64_t exch_oid = 0;
        if (auto oi = d.find("ordId"); oi != d.end()) {
            std::string oid_str(oi->value().as_string());
            if (!oid_str.empty())
                exch_oid = static_cast<uint64_t>(std::stoull(oid_str));
        }
        ygg::log::info("[Heimdall] OKXExecParser: order acked cloid={} ordId={}", cloid, exch_oid);
        // ACKED events come from the orders channel — skip emitting here.
    } else {
        ygg::log::error("[Heimdall] OKXExecParser: order rejected cloid={} sCode={} sMsg={}", cloid, s_code, s_msg);
        ExecEvent ev{};
        ev.order_id = order_id;
        ev.exchange_id = bpt::messages::ExchangeId::OKX;
        ev.instrument_id = 0;
        ev.local_ts_ns = recv_ns;
        ev.exchange_ts_ns = recv_ns;
        ev.side = bpt::messages::OrderSide::BUY;  // placeholder; strategy knows side
        ev.status = ES::REJECTED;
        ev.reject_reason = RR::EXCHANGE_ERROR;
        if (on_exec_event)
            on_exec_event(ev);
    }
}

void OKXExecParser::handle_orders_channel_item(const json::object& d, uint64_t recv_ns) {
    using ES = bpt::messages::ExecStatus;
    using OS = bpt::messages::OrderSide;
    using OT = bpt::messages::OrderType;
    using RR = bpt::messages::RejectReason;

    std::string cloid = std::string(d.at("clOrdId").as_string());
    uint64_t order_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cloid_to_order_id_.find(cloid);
        if (it != cloid_to_order_id_.end())
            order_id = it->second;
    }
    if (order_id == 0) {
        ygg::log::warn("[Heimdall] OKXExecParser: unknown cloid={}", cloid);
        return;
    }

    ExecEvent ev{};
    ev.order_id = order_id;
    ev.exchange_order_id = static_cast<uint64_t>(std::stoull(std::string(d.at("ordId").as_string())));
    ev.exchange_id = bpt::messages::ExchangeId::OKX;
    ev.instrument_id = 0;
    ev.local_ts_ns = recv_ns;
    ev.reject_reason = RR::OK;

    std::string side_str = std::string(d.at("side").as_string());
    ev.side = (side_str == "buy") ? OS::BUY : OS::SELL;

    std::string ord_type = std::string(d.at("ordType").as_string());
    if (ord_type == "market")
        ev.order_type = OT::MARKET;
    else if (ord_type == "post_only")
        ev.order_type = OT::POST_ONLY;
    else
        ev.order_type = OT::LIMIT;

    ev.price = static_cast<int64_t>(std::stod(std::string(d.at("px").as_string())) * kPriceScale);

    {
        std::string inst_id_str = std::string(d.at("instId").as_string());
        double ctval = 1.0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (auto it = contract_sizes_.find(inst_id_str); it != contract_sizes_.end())
                ctval = it->second;
        }
        double fill_contracts = std::stod(std::string(d.at("fillSz").as_string()));
        double total_contracts = std::stod(std::string(d.at("sz").as_string()));
        ev.filled_qty = static_cast<uint64_t>(std::round(fill_contracts * ctval * kQtyScale));
        uint64_t total_qty = static_cast<uint64_t>(std::round(total_contracts * ctval * kQtyScale));
        ev.remaining_qty = total_qty > ev.filled_qty ? total_qty - ev.filled_qty : 0;
    }

    ev.fee = static_cast<int64_t>(std::stod(std::string(d.at("fee").as_string())) * kPriceScale);
    ev.fee_currency = parse_fee_ccy(std::string(d.at("feeCcy").as_string()));

    if (auto tsit = d.find("uTime"); tsit != d.end())
        ev.exchange_ts_ns = static_cast<uint64_t>(std::stoull(std::string(tsit->value().as_string()))) * 1000000ULL;
    else
        ev.exchange_ts_ns = recv_ns;

    std::string state = std::string(d.at("state").as_string());
    if (state == "live")
        ev.status = ES::ACKED;
    else if (state == "partially_filled")
        ev.status = ES::PARTIAL;
    else if (state == "filled")
        ev.status = ES::FILLED;
    else if (state == "canceled")
        ev.status = ES::CANCELLED;
    else {
        ev.status = ES::REJECTED;
        ev.reject_reason = RR::EXCHANGE_ERROR;
    }

    // Suppress duplicate ACKED events from orders-channel snapshot replay on reconnect.
    if (ev.status == ES::ACKED) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!acked_orders_.insert(ev.order_id).second) {
            ygg::log::debug("[Heimdall] OKXExecParser: suppressed duplicate ACKED order_id={}", ev.order_id);
            return;
        }
    } else if (ev.status == ES::CANCELLED) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cancelled_orders_.insert(ev.order_id).second) {
            ygg::log::debug("[Heimdall] OKXExecParser: suppressed duplicate CANCELLED order_id={}", ev.order_id);
            return;
        }
        acked_orders_.erase(ev.order_id);
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        acked_orders_.erase(ev.order_id);
    }

    if (on_exec_event)
        on_exec_event(ev);
}

}  // namespace bpt::order_gateway::adapter
