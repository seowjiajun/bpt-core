#include "order_gateway/adapter/deribit/deribit_exec_parser.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <functional>
#include <string>
#include <yggdrasil/logging.h>

namespace bpt::order_gateway::adapter {

namespace json = boost::json;

static constexpr double kScale = 1e8;

static bpt::messages::FeeCurrency::Value parse_fee_ccy(const std::string& instrument_name) {
    if (instrument_name.find("BTC") == 0)
        return bpt::messages::FeeCurrency::BTC;
    if (instrument_name.find("ETH") == 0)
        return bpt::messages::FeeCurrency::ETH;
    return bpt::messages::FeeCurrency::USDT;
}

void DeribitExecParser::register_order(const std::string& label, uint64_t order_id) {
    std::lock_guard<std::mutex> lk(mu_);
    label_to_order_id_[label] = order_id;
}

std::string DeribitExecParser::get_exchange_order_id(uint64_t order_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = order_id_to_exch_oid_.find(order_id);
    return (it != order_id_to_exch_oid_.end()) ? it->second : std::string{};
}

void DeribitExecParser::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    acked_orders_.clear();
    cancelled_orders_.clear();
}

void DeribitExecParser::handle_subscription_event(const json::object& d, uint64_t recv_ns) {
    using ES = bpt::messages::ExecStatus;
    using OS = bpt::messages::OrderSide;
    using OT = bpt::messages::OrderType;
    using RR = bpt::messages::RejectReason;

    std::string label;
    if (auto lit = d.find("label"); lit != d.end() && lit->value().is_string())
        label = std::string(lit->value().as_string());

    uint64_t order_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = label_to_order_id_.find(label);
        if (it != label_to_order_id_.end())
            order_id = it->second;
    }
    if (order_id == 0) {
        ygg::log::debug("[Heimdall] DeribitExecParser: subscription unknown label={}", label);
        return;
    }

    ExecEvent ev{};
    ev.order_id = order_id;
    ev.exchange_id = bpt::messages::ExchangeId::DERIBIT;
    ev.instrument_id = 0;
    ev.local_ts_ns = recv_ns;
    ev.reject_reason = RR::OK;

    // Exchange order ID
    std::string exch_oid;
    if (auto oit = d.find("order_id"); oit != d.end() && oit->value().is_string())
        exch_oid = std::string(oit->value().as_string());
    if (!exch_oid.empty()) {
        try {
            ev.exchange_order_id = static_cast<uint64_t>(std::stoull(exch_oid));
        } catch (...) {
            ev.exchange_order_id = std::hash<std::string>{}(exch_oid);
        }
        std::lock_guard<std::mutex> lk(mu_);
        exch_oid_to_order_id_[exch_oid] = order_id;
        order_id_to_exch_oid_[order_id] = exch_oid;
    }

    if (auto dit = d.find("direction"); dit != d.end() && dit->value().is_string())
        ev.side = (std::string(dit->value().as_string()) == "buy") ? OS::BUY : OS::SELL;

    if (auto tit = d.find("order_type"); tit != d.end() && tit->value().is_string())
        ev.order_type = (std::string(tit->value().as_string()) == "market") ? OT::MARKET : OT::LIMIT;

    if (auto pit = d.find("price"); pit != d.end() && pit->value().is_number())
        ev.price = static_cast<int64_t>(pit->value().to_number<double>() * kScale);
    else if (auto pit2 = d.find("average_price"); pit2 != d.end() && pit2->value().is_number())
        ev.price = static_cast<int64_t>(pit2->value().to_number<double>() * kScale);

    double filled_amount = 0.0;
    if (auto fit = d.find("filled_amount"); fit != d.end() && fit->value().is_number())
        filled_amount = fit->value().to_number<double>();
    ev.filled_qty = static_cast<uint64_t>(filled_amount * kScale);

    double total_amount = 0.0;
    if (auto ait = d.find("amount"); ait != d.end() && ait->value().is_number())
        total_amount = ait->value().to_number<double>();
    uint64_t total_qty = static_cast<uint64_t>(total_amount * kScale);
    ev.remaining_qty = total_qty > ev.filled_qty ? total_qty - ev.filled_qty : 0;

    if (auto feit = d.find("commission"); feit != d.end() && feit->value().is_number())
        ev.fee = static_cast<int64_t>(feit->value().to_number<double>() * kScale);

    std::string instrument_name;
    if (auto init = d.find("instrument_name"); init != d.end() && init->value().is_string())
        instrument_name = std::string(init->value().as_string());
    ev.fee_currency = parse_fee_ccy(instrument_name);

    if (auto tsit = d.find("last_update_timestamp"); tsit != d.end() && tsit->value().is_number())
        ev.exchange_ts_ns = static_cast<uint64_t>(tsit->value().to_number<uint64_t>()) * 1000000ULL;
    else
        ev.exchange_ts_ns = recv_ns;

    std::string state;
    if (auto sit = d.find("order_state"); sit != d.end() && sit->value().is_string())
        state = std::string(sit->value().as_string());

    if (state == "open") {
        ev.status = (filled_amount > 0.0) ? ES::PARTIAL : ES::ACKED;
    } else if (state == "filled") {
        ev.status = ES::FILLED;
    } else if (state == "cancelled") {
        ev.status = ES::CANCELLED;
    } else if (state == "rejected") {
        ev.status = ES::REJECTED;
        ev.reject_reason = RR::EXCHANGE_ERROR;
    } else if (state == "untriggered" || state == "triggered") {
        ev.status = ES::ACKED;
    } else {
        ev.status = ES::REJECTED;
        ev.reject_reason = RR::EXCHANGE_ERROR;
    }

    // Suppress duplicates on reconnect
    if (ev.status == ES::ACKED) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!acked_orders_.insert(ev.order_id).second) {
            ygg::log::debug("[Heimdall] DeribitExecParser: suppressed duplicate ACKED order_id={}", ev.order_id);
            return;
        }
    } else if (ev.status == ES::CANCELLED) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cancelled_orders_.insert(ev.order_id).second) {
            ygg::log::debug("[Heimdall] DeribitExecParser: suppressed duplicate CANCELLED order_id={}", ev.order_id);
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

void DeribitExecParser::handle_order_response(const json::object& order_obj, uint64_t recv_ns) {
    using ES = bpt::messages::ExecStatus;
    using OS = bpt::messages::OrderSide;
    using OT = bpt::messages::OrderType;
    using RR = bpt::messages::RejectReason;

    std::string label;
    if (auto lit = order_obj.find("label"); lit != order_obj.end() && lit->value().is_string())
        label = std::string(lit->value().as_string());

    uint64_t order_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = label_to_order_id_.find(label);
        if (it != label_to_order_id_.end())
            order_id = it->second;
    }
    if (order_id == 0) {
        ygg::log::warn("[Heimdall] DeribitExecParser: order response unknown label={}", label);
        return;
    }

    std::string exch_oid;
    if (auto oit = order_obj.find("order_id"); oit != order_obj.end() && oit->value().is_string())
        exch_oid = std::string(oit->value().as_string());
    if (!exch_oid.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        exch_oid_to_order_id_[exch_oid] = order_id;
        order_id_to_exch_oid_[order_id] = exch_oid;
    }

    std::string order_state;
    if (auto sit = order_obj.find("order_state"); sit != order_obj.end() && sit->value().is_string())
        order_state = std::string(sit->value().as_string());

    ygg::log::info("[Heimdall] DeribitExecParser: order response label={} exchange_oid={} state={}",
                   label,
                   exch_oid,
                   order_state);

    // Only emit for terminal IOC states — GTC orders are reported via subscription channel.
    if (order_state != "filled" && order_state != "cancelled" && order_state != "rejected")
        return;

    ExecEvent ev{};
    ev.order_id = order_id;
    if (!exch_oid.empty()) {
        try {
            ev.exchange_order_id = static_cast<uint64_t>(std::stoull(exch_oid));
        } catch (...) {
            ev.exchange_order_id = std::hash<std::string>{}(exch_oid);
        }
    }
    ev.exchange_id = bpt::messages::ExchangeId::DERIBIT;
    ev.local_ts_ns = recv_ns;
    ev.reject_reason = RR::OK;
    ev.order_type = OT::LIMIT;

    if (auto dit = order_obj.find("direction"); dit != order_obj.end() && dit->value().is_string())
        ev.side = (std::string(dit->value().as_string()) == "buy") ? OS::BUY : OS::SELL;

    if (auto pit = order_obj.find("average_price"); pit != order_obj.end() && pit->value().is_number())
        ev.price = static_cast<int64_t>(pit->value().to_number<double>() * kScale);

    if (auto fit = order_obj.find("filled_amount"); fit != order_obj.end() && fit->value().is_number())
        ev.filled_qty = static_cast<uint64_t>(fit->value().to_number<double>() * kScale);

    if (auto tit = order_obj.find("last_update_timestamp"); tit != order_obj.end() && tit->value().is_number())
        ev.exchange_ts_ns = static_cast<uint64_t>(tit->value().to_number<double>()) * 1000000ULL;
    else
        ev.exchange_ts_ns = recv_ns;

    if (order_state == "filled") {
        ev.status = ES::FILLED;
        ev.remaining_qty = 0;
    } else if (order_state == "cancelled") {
        ev.status = ES::CANCELLED;
        if (auto rit = order_obj.find("amount"); rit != order_obj.end() && rit->value().is_number()) {
            double total = rit->value().to_number<double>();
            double filled = static_cast<double>(ev.filled_qty) / kScale;
            ev.remaining_qty = static_cast<uint64_t>((total - filled) * kScale);
        }
    } else {
        ev.status = ES::REJECTED;
        ev.reject_reason = RR::EXCHANGE_ERROR;
    }

    if (on_exec_event)
        on_exec_event(ev);
}

}  // namespace bpt::order_gateway::adapter
