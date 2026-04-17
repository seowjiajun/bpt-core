#include "order_gateway/adapter/okx/okx_action_codec.h"

#include <boost/json.hpp>
#include <string>

namespace bpt::order_gateway::adapter::okx {

namespace json = boost::json;

namespace {
constexpr double kPriceScale = 1e8;
// Internal wire scale is 1e8 across all adapters and the SBE protocol —
// matches Binance, Hyperliquid, Deribit, and order.quantity() directly.
// Before the fix this was 1e5 which made every OKX order 1000x too large.
constexpr double kQtyScale   = 1e8;

const char* side_str(bpt::messages::OrderSide::Value s) {
    using OS = bpt::messages::OrderSide;
    return (s == OS::BUY) ? "buy" : "sell";
}

const char* ord_type_str(bpt::messages::OrderType::Value t) {
    using OT = bpt::messages::OrderType;
    if (t == OT::MARKET)    return "market";
    if (t == OT::POST_ONLY) return "post_only";
    return "limit";
}

const char* tif_str(bpt::messages::TimeInForce::Value tif) {
    using TIF = bpt::messages::TimeInForce;
    switch (tif) {
        case TIF::IOC: return "ioc";
        case TIF::FOK: return "fok";
        default:       return "gtc";
    }
}

// Convert fenrir's base-currency qty (base * 1e8) to OKX's wire size
// (contracts). SWAP/FUTURES: sz = qty_base / ctVal. SPOT/MARGIN fall
// through to ctVal = 1.0.
std::string size_to_contracts(uint64_t quantity_e8,
                              std::string_view inst_id,
                              const ContractSizes& contract_sizes) {
    const double qty_base = static_cast<double>(quantity_e8) / kQtyScale;
    double ctval = 1.0;
    if (auto it = contract_sizes.find(std::string(inst_id)); it != contract_sizes.end())
        ctval = it->second;
    return std::to_string(qty_base / ctval);
}

// SWAP and FUTURES are cross-margin by default; SPOT is cash. OKX will
// reject the wrong tdMode with a sCode error so this classification is
// load-bearing.
const char* td_mode_for(std::string_view inst_id) {
    if (inst_id.find("-SWAP") != std::string_view::npos ||
        inst_id.find("-FUTURES") != std::string_view::npos) {
        return "cross";
    }
    return "cash";
}

}  // namespace

json::value build_order_action(const OrderSpec& spec,
                               uint64_t req_id,
                               const InstIdCodeMap& inst_id_codes,
                               const ContractSizes& contract_sizes) {
    json::object arg;
    arg["instId"] = spec.inst_id;

    // WebSocket trading endpoint requires instIdCode in addition to
    // instId — the REST /order path would accept instId alone, but
    // wseeapap (the WS equivalent) enforces the numeric id lookup.
    if (auto it = inst_id_codes.find(spec.inst_id); it != inst_id_codes.end())
        arg["instIdCode"] = it->second;

    arg["tdMode"]  = td_mode_for(spec.inst_id);
    arg["side"]    = side_str(spec.side);
    arg["ordType"] = ord_type_str(spec.order_type);
    (void)spec.tif;  // OKX infers tif from ordType (post_only, ioc, fok, limit=gtc)

    arg["sz"]      = size_to_contracts(spec.quantity_e8, spec.inst_id, contract_sizes);
    arg["clOrdId"] = spec.cloid;

    if (spec.order_type != bpt::messages::OrderType::MARKET) {
        arg["px"] = std::to_string(static_cast<double>(spec.price_e8) / kPriceScale);
    }

    json::object msg;
    msg["id"] = "r" + std::to_string(req_id);
    msg["op"] = "order";
    msg["args"] = json::array{std::move(arg)};
    return msg;
}

json::value build_cancel_action(std::string_view inst_id,
                                 std::string_view cloid,
                                 uint64_t req_id) {
    json::object arg;
    arg["instId"]  = std::string(inst_id);
    arg["clOrdId"] = std::string(cloid);

    json::object msg;
    msg["id"] = "c" + std::to_string(req_id);
    msg["op"] = "cancel-order";
    msg["args"] = json::array{std::move(arg)};
    return msg;
}

json::value build_modify_action(std::string_view inst_id,
                                 std::string_view cloid,
                                 int64_t new_price_e8,
                                 uint64_t new_quantity_e8,
                                 const ContractSizes& contract_sizes) {
    json::object arg;
    arg["instId"]  = std::string(inst_id);
    arg["clOrdId"] = std::string(cloid);
    arg["newPx"]   = std::to_string(static_cast<double>(new_price_e8) / kPriceScale);
    arg["newSz"]   = size_to_contracts(new_quantity_e8, inst_id, contract_sizes);

    json::object msg;
    msg["op"] = "amend-order";
    msg["args"] = json::array{std::move(arg)};
    return msg;
}

}  // namespace bpt::order_gateway::adapter::okx
