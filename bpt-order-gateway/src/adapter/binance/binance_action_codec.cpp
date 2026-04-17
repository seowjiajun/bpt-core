#include "order_gateway/adapter/binance/binance_action_codec.h"

#include <string>

namespace bpt::order_gateway::adapter::binance {

namespace {
constexpr double kScale = 1e8;

const char* side_str(bpt::messages::OrderSide::Value s) {
    using OS = bpt::messages::OrderSide;
    return (s == OS::BUY) ? "BUY" : "SELL";
}

const char* type_str(bpt::messages::OrderType::Value t) {
    using OT = bpt::messages::OrderType;
    if (t == OT::MARKET)    return "MARKET";
    if (t == OT::POST_ONLY) return "LIMIT_MAKER";
    return "LIMIT";
}

const char* tif_str(bpt::messages::TimeInForce::Value tif) {
    using TIF = bpt::messages::TimeInForce;
    switch (tif) {
        case TIF::IOC: return "IOC";
        case TIF::FOK: return "FOK";
        default:       return "GTC";
    }
}
}  // namespace

std::string build_new_order_params(const OrderSpec& spec) {
    using OT = bpt::messages::OrderType;

    std::string params = "symbol=" + spec.symbol +
                         "&side=" + side_str(spec.side) +
                         "&type=" + type_str(spec.order_type) +
                         "&quantity=" + std::to_string(static_cast<double>(spec.quantity_e8) / kScale) +
                         "&newClientOrderId=" + spec.cloid;

    if (spec.order_type != OT::MARKET) {
        params += "&timeInForce=";
        params += tif_str(spec.tif);
        params += "&price=" + std::to_string(static_cast<double>(spec.price_e8) / kScale);
    }
    return params;
}

std::string build_cancel_params(std::string_view symbol, std::string_view cloid) {
    std::string s;
    s += "symbol=";
    s += symbol;
    s += "&origClientOrderId=";
    s += cloid;
    return s;
}

std::string build_modify_replace_params(std::string_view symbol,
                                         std::string_view new_cloid,
                                         int64_t new_price_e8,
                                         uint64_t new_quantity_e8) {
    std::string s;
    s += "symbol=";
    s += symbol;
    // side/type/tif hardcoded to match pre-refactor behaviour — see header.
    s += "&side=BUY&type=LIMIT&timeInForce=GTC";
    s += "&quantity=" + std::to_string(static_cast<double>(new_quantity_e8) / kScale);
    s += "&price=" + std::to_string(static_cast<double>(new_price_e8) / kScale);
    s += "&newClientOrderId=";
    s += new_cloid;
    return s;
}

}  // namespace bpt::order_gateway::adapter::binance
