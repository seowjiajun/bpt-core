#include "heimdall/adapter/hyperliquid/hyperliquid_action_codec.h"

#include <boost/json.hpp>
#include <cmath>
#include <cstdio>

namespace heimdall::adapter::hyperliquid {

namespace json = boost::json;

std::string float_to_wire(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.8f", v);
    std::string s(buf);
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last == dot)
            s.erase(dot);           // "50000." → "50000"
        else
            s.erase(last + 1);      // "72198.05750000" → "72198.0575"
    }
    if (s == "-0") s = "0";
    return s;
}

AssetMeta lookup_testnet_asset(std::string_view exchange_symbol) {
    // Hardcoded for now — mainnet will have different asset_idx values.
    // TODO(prod): load from /info meta at startup and drop this table.
    if (exchange_symbol == "BTC") return {3, 5, 1};
    if (exchange_symbol == "ETH") return {4, 4, 2};
    return {-1, 0, 0};
}

namespace {

// Apply HL's price precision rules to a natural-units price.
// For BTC (sz_decimals = 5) the result is an integer — max 5 sig figs
// and max (6 - sz_decimals) = 1 decimal place means at ~$72k there's
// no valid non-integer form. For assets with higher max_px_decimals
// the round target is different.
double round_price_for_asset(double price, const AssetMeta& meta) {
    // Simple case today: round to integer. Extend this when we add a
    // non-integer-priced asset. Sentinel (-1) means unknown — return as-is.
    if (meta.asset_idx < 0) return price;
    return std::round(price);
}

// Build the inner order object ({"a","b","p","s","r","t":{"limit":{"tif":...}}})
// used by both the new-order and modify actions.
json::object build_order_object(std::string_view exchange_symbol,
                                bool is_buy,
                                double price_natural,
                                double size_natural,
                                HlTif tif) {
    const AssetMeta meta = lookup_testnet_asset(exchange_symbol);
    const double px = round_price_for_asset(price_natural, meta);

    json::object o;
    o["a"] = meta.asset_idx;
    o["b"] = is_buy;
    o["p"] = float_to_wire(px);
    o["s"] = float_to_wire(size_natural);
    o["r"] = false;
    o["t"] = json::object{{"limit", json::object{{"tif", tif_to_string(tif)}}}};
    return o;
}

}  // namespace

const char* tif_to_string(HlTif tif) {
    switch (tif) {
        case HlTif::Alo: return "Alo";
        case HlTif::Ioc: return "Ioc";
        case HlTif::Gtc: return "Gtc";
    }
    return "Gtc";
}

json::value build_order_action(std::string_view exchange_symbol,
                               bool is_buy,
                               double price_natural,
                               double size_natural,
                               HlTif tif) {
    json::object action;
    action["type"] = "order";
    action["orders"] = json::array{build_order_object(exchange_symbol, is_buy, price_natural, size_natural, tif)};
    action["grouping"] = "na";
    return action;
}

json::value build_cancel_action(std::string_view exchange_symbol, uint64_t exch_oid) {
    const AssetMeta meta = lookup_testnet_asset(exchange_symbol);

    json::object c;
    c["a"] = meta.asset_idx;
    c["o"] = exch_oid;

    json::object action;
    action["type"] = "cancel";
    action["cancels"] = json::array{std::move(c)};
    return action;
}

json::value build_modify_action(std::string_view exchange_symbol,
                                uint64_t client_or_exch_oid,
                                double price_natural,
                                double size_natural) {
    // ModifyOrder doesn't carry a side, so we default to buy — upstream
    // order-state tracking should only trigger modifies on in-book orders
    // and this field is ignored server-side when oid is set.
    json::object ord_inner = build_order_object(exchange_symbol,
                                                /*is_buy=*/true,
                                                price_natural,
                                                size_natural,
                                                HlTif::Gtc);

    json::object m;
    m["oid"] = client_or_exch_oid;
    m["order"] = std::move(ord_inner);

    json::object action;
    action["type"] = "modify";
    action["modifies"] = json::array{std::move(m)};
    return action;
}

json::value build_schedule_cancel_action(int64_t time_ms) {
    json::object action;
    action["type"] = "scheduleCancel";
    if (time_ms > 0)
        action["time"] = time_ms;
    return action;
}

}  // namespace heimdall::adapter::hyperliquid
