#include "heimdall/adapter/deribit/deribit_action_codec.h"

#include <boost/json.hpp>

namespace heimdall::adapter::deribit {

namespace json = boost::json;

namespace {
constexpr double kScale = 1e8;

const char* type_str(bifrost::protocol::OrderType::Value t) {
    using OT = bifrost::protocol::OrderType;
    if (t == OT::MARKET) return "market";
    return "limit";
}

const char* tif_str(bifrost::protocol::TimeInForce::Value tif) {
    using TIF = bifrost::protocol::TimeInForce;
    switch (tif) {
        case TIF::IOC: return "immediate_or_cancel";
        case TIF::FOK: return "fill_or_kill";
        default:       return "good_til_cancelled";
    }
}

std::string serialise_rpc(std::string_view method, json::object params, uint64_t req_id) {
    json::object msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = req_id;
    msg["method"] = std::string(method);
    msg["params"] = std::move(params);
    return json::serialize(msg);
}
}  // namespace

std::string build_auth_msg(std::string_view client_id,
                            std::string_view client_secret,
                            uint64_t req_id) {
    json::object params;
    params["grant_type"] = "client_credentials";
    params["client_id"] = std::string(client_id);
    params["client_secret"] = std::string(client_secret);
    return serialise_rpc("public/auth", std::move(params), req_id);
}

std::string build_new_order_msg(const OrderSpec& spec, uint64_t req_id) {
    using OT = bifrost::protocol::OrderType;
    using OS = bifrost::protocol::OrderSide;

    const double price = static_cast<double>(spec.price_e8) / kScale;
    const double amount = static_cast<double>(spec.quantity_e8) / kScale;

    json::object params;
    params["instrument_name"] = spec.instrument_name;
    params["amount"] = amount;
    params["type"] = type_str(spec.order_type);
    params["label"] = spec.label;
    if (spec.order_type != OT::MARKET) {
        params["price"] = price;
        params["time_in_force"] = tif_str(spec.tif);
    }
    const char* method = (spec.side == OS::BUY) ? "private/buy" : "private/sell";
    return serialise_rpc(method, std::move(params), req_id);
}

std::string build_cancel_msg(std::string_view exchange_order_id, uint64_t req_id) {
    json::object params;
    params["order_id"] = std::string(exchange_order_id);
    return serialise_rpc("private/cancel", std::move(params), req_id);
}

std::string build_edit_msg(std::string_view exchange_order_id,
                            int64_t new_price_e8,
                            uint64_t new_quantity_e8,
                            uint64_t req_id) {
    json::object params;
    params["order_id"] = std::string(exchange_order_id);
    params["amount"] = static_cast<double>(new_quantity_e8) / kScale;
    params["price"] = static_cast<double>(new_price_e8) / kScale;
    return serialise_rpc("private/edit", std::move(params), req_id);
}

std::string build_test_response(uint64_t req_id) {
    return serialise_rpc("public/test", json::object{}, req_id);
}

std::string build_simple_rpc(std::string_view method,
                              const std::string& params_json,
                              uint64_t req_id) {
    json::object params;
    if (!params_json.empty()) {
        auto parsed = json::parse(params_json);
        if (parsed.is_object())
            params = parsed.as_object();
    }
    return serialise_rpc(method, std::move(params), req_id);
}

}  // namespace heimdall::adapter::deribit
