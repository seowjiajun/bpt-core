#include "order_gateway/adapter/hyperliquid/hyperliquid_action_encoder.h"

#include <algorithm>
#include <boost/json.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace bpt::order_gateway::adapter::hyperliquid {

namespace json = boost::json;

std::string float_to_wire(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.8f", v);
    std::string s(buf);
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last == dot)
            s.erase(dot);  // "50000." → "50000"
        else
            s.erase(last + 1);  // "72198.05750000" → "72198.0575"
    }
    if (s == "-0")
        s = "0";
    return s;
}

AssetTable parse_spot_universe_meta(std::string_view body) {
    // HL spotMeta shape (truncated):
    //   {"tokens": [{"name":"USDC","szDecimals":8,"index":0,...},
    //               {"name":"PURR","szDecimals":0,"index":1,...}],
    //    "universe": [{"name":"PURR/USDC","tokens":[1,0],"index":0,...},
    //                 {"name":"@1","tokens":[2,0],"index":1,...}]}
    // asset_idx for spot orders = 10000 + universe[i].index (HL convention).
    // Tick precision uses the spot rule: max_px_decimals = 8 - base.szDecimals.
    const json::value parsed = json::parse(body);
    if (!parsed.is_object())
        throw std::runtime_error("HL spotMeta response not an object");
    const auto& obj = parsed.as_object();
    const auto* tokens_p = obj.if_contains("tokens");
    const auto* universe_p = obj.if_contains("universe");
    if (!tokens_p || !tokens_p->is_array())
        throw std::runtime_error("HL spotMeta response missing 'tokens' array");
    if (!universe_p || !universe_p->is_array())
        throw std::runtime_error("HL spotMeta response missing 'universe' array");

    // Index tokens by their `index` field — HL leaves gaps when delisting,
    // so position-in-array is unreliable.
    std::unordered_map<int64_t, int> token_sz_decimals;
    for (const auto& t : tokens_p->as_array()) {
        if (!t.is_object())
            continue;
        const auto& to = t.as_object();
        const auto* idx = to.if_contains("index");
        const auto* sz = to.if_contains("szDecimals");
        if (!idx || !idx->is_int64() || !sz || !sz->is_int64())
            continue;
        token_sz_decimals.emplace(idx->as_int64(), static_cast<int>(sz->as_int64()));
    }

    AssetTable out;
    const auto& uarr = universe_p->as_array();
    out.reserve(uarr.size());
    for (const auto& u : uarr) {
        if (!u.is_object())
            continue;
        const auto& uo = u.as_object();
        const auto* name = uo.if_contains("name");
        const auto* index = uo.if_contains("index");
        const auto* tokens_ref = uo.if_contains("tokens");
        if (!name || !name->is_string() || !index || !index->is_int64() || !tokens_ref || !tokens_ref->is_array() ||
            tokens_ref->as_array().empty())
            continue;
        const auto& tokens_arr = tokens_ref->as_array();
        if (!tokens_arr[0].is_int64())
            continue;
        const int64_t base_idx = tokens_arr[0].as_int64();
        const auto it = token_sz_decimals.find(base_idx);
        if (it == token_sz_decimals.end())
            continue;

        AssetMeta meta;
        meta.asset_idx = 10000 + static_cast<int>(index->as_int64());
        meta.sz_decimals = it->second;
        meta.max_px_decimals = 8 - meta.sz_decimals;
        out.emplace(std::string(name->as_string()), meta);
    }
    return out;
}

AssetTable parse_universe_meta(std::string_view meta_response_body) {
    // HL /info meta response shape (truncated):
    //   {"universe": [
    //     {"name": "BTC", "szDecimals": 5, "maxLeverage": 40, ...},
    //     {"name": "ETH", "szDecimals": 4, "maxLeverage": 25, ...},
    //     ...
    //   ], ...}
    // Asset index = position in the universe array (0-based).
    // maxPxDecimals = 6 - szDecimals for perps (HL's rule: max 5 sig figs,
    // max (6 - szDecimals) decimal places).
    const json::value parsed = json::parse(meta_response_body);
    if (!parsed.is_object())
        throw std::runtime_error("HL meta response not an object");
    const auto* universe = parsed.as_object().if_contains("universe");
    if (!universe || !universe->is_array())
        throw std::runtime_error("HL meta response missing 'universe' array");

    AssetTable out;
    const auto& arr = universe->as_array();
    out.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].is_object())
            continue;
        const auto& obj = arr[i].as_object();
        const auto* name = obj.if_contains("name");
        const auto* sz = obj.if_contains("szDecimals");
        if (!name || !name->is_string() || !sz || !sz->is_int64())
            continue;
        const int sz_dec = static_cast<int>(sz->as_int64());
        AssetMeta meta;
        meta.asset_idx = static_cast<int>(i);
        meta.sz_decimals = sz_dec;
        meta.max_px_decimals = 6 - sz_dec;
        out.emplace(std::string(name->as_string()), meta);
    }
    return out;
}

namespace {

// Apply HL's price precision rules to a natural-units price.
// HL rule: max 5 significant figures AND max (6 - szDecimals) decimal
// places. For BTC (szDecimals=5) the decimal cap = 1 but the 5-sigfig
// cap dominates at ~$72k, forcing integer prices. For XMR (szDecimals=2)
// the decimal cap = 4 but at ~$385 only 2 decimals survive the 5-sigfig
// rule. This function picks the stricter of the two caps.
double round_price_for_asset(double price, const AssetMeta& meta) {
    if (price <= 0.0)
        return price;
    // 5-significant-figure cap: number of digits before the decimal.
    const int int_digits = static_cast<int>(std::floor(std::log10(price))) + 1;
    const int sigfig_decimals = std::max(0, 5 - int_digits);
    const int decimals = std::min(sigfig_decimals, meta.max_px_decimals);
    const double scale = std::pow(10.0, decimals);
    return std::round(price * scale) / scale;
}

// Build the inner order object ({"a","b","p","s","r","t":{"limit":{"tif":...}}})
// used by both the new-order and modify actions.
json::object build_order_object(const AssetMeta& meta,
                                bool is_buy,
                                double price_natural,
                                double size_natural,
                                HlTif tif) {
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
        case HlTif::Alo:
            return "Alo";
        case HlTif::Ioc:
            return "Ioc";
        case HlTif::Gtc:
            return "Gtc";
    }
    return "Gtc";
}

json::value build_order_action(const AssetMeta& meta,
                               bool is_buy,
                               double price_natural,
                               double size_natural,
                               HlTif tif) {
    json::object action;
    action["type"] = "order";
    action["orders"] = json::array{build_order_object(meta, is_buy, price_natural, size_natural, tif)};
    action["grouping"] = "na";
    return action;
}

json::value build_cancel_action(const AssetMeta& meta, uint64_t exch_oid) {
    json::object c;
    c["a"] = meta.asset_idx;
    c["o"] = exch_oid;

    json::object action;
    action["type"] = "cancel";
    action["cancels"] = json::array{std::move(c)};
    return action;
}

json::value build_modify_action(const AssetMeta& meta,
                                uint64_t client_or_exch_oid,
                                double price_natural,
                                double size_natural) {
    // ModifyOrder doesn't carry a side, so we default to buy — upstream
    // order-state tracking should only trigger modifies on in-book orders
    // and this field is ignored server-side when oid is set.
    json::object ord_inner = build_order_object(meta,
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

}  // namespace bpt::order_gateway::adapter::hyperliquid
