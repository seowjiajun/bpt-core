#include "bridge/ws/message_encoder.h"

#include <cmath>
#include <nlohmann/json.hpp>

namespace bpt::bridge::encode {

using nlohmann::json;

namespace {

// nlohmann/json refuses to serialise NaN as a JSON number; encode it as `null`
// so the frontend can use `value == null` to mean "not computed yet" rather
// than collapsing to 0 or a sentinel double.
json finite_or_null(double v) {
    return std::isfinite(v) ? json(v) : json(nullptr);
}

}  // namespace

std::string session(std::string_view symbol,
                    std::string_view strategy,
                    std::string_view exchange,
                    std::string_view mode,
                    std::string_view instrument_type) {
    return json{
        {"type", "session"},
        {"symbol", symbol},
        {"strategy", strategy},
        {"exchange", exchange},
        {"mode", mode},
        {"instrumentType", instrument_type},
    }
        .dump();
}

std::string status(std::string_view state) {
    return json{
        {"type", "status"},
        {"state", state},
    }
        .dump();
}

std::string tick(uint64_t ts_ns, std::string_view symbol, double price) {
    return json{
        {"type", "tick"},
        {"ts", ts_ns},
        {"symbol", symbol},
        {"price", price},
    }
        .dump();
}

std::string fill(uint64_t ts_ns,
                 uint64_t order_id,
                 std::string_view symbol,
                 Side side,
                 std::string_view order_type,
                 double qty,
                 double price,
                 double fee,
                 double realized_pnl,
                 double equity) {
    return json{
        {"type", "fill"},
        {"ts", ts_ns},
        {"orderId", order_id},
        {"symbol", symbol},
        {"side", side == Side::Buy ? "BUY" : "SELL"},
        {"orderType", order_type},
        {"qty", qty},
        {"price", price},
        {"fee", fee},
        {"realizedPnl", realized_pnl},
        {"equity", equity},
    }
        .dump();
}

std::string position(std::string_view symbol, double net_qty, double avg_entry, double unrealized_pnl) {
    return json{
        {"type", "position"},
        {"symbol", symbol},
        {"netQty", net_qty},
        {"avgEntry", avg_entry},
        {"unrealizedPnl", unrealized_pnl},
    }
        .dump();
}

std::string account(uint64_t ts_ns,
                    double balance,
                    double equity,
                    const std::vector<AccountPosition>& positions,
                    const std::vector<AccountCurrencyBalance>& currency_balances) {
    auto positions_json = json::array();
    for (const auto& p : positions) {
        positions_json.push_back({
            {"symbol", p.symbol},
            {"netQty", p.net_qty},
            {"avgEntry", p.avg_entry},
            {"unrealizedPnl", p.unrealized_pnl},
        });
    }
    auto currency_balances_json = json::array();
    for (const auto& cb : currency_balances) {
        currency_balances_json.push_back({
            {"ccy", cb.ccy},
            {"equity", cb.equity},
            {"availableBalance", cb.available_balance},
        });
    }
    return json{
        {"type", "account"},
        {"ts", ts_ns},
        {"balance", balance},
        {"equity", equity},
        {"positions", std::move(positions_json)},
        {"currencyBalances", std::move(currency_balances_json)},
    }
        .dump();
}

std::string order(uint64_t ts_ns,
                  uint64_t order_id,
                  std::string_view symbol,
                  Side side,
                  std::string_view order_type,
                  double price,
                  double qty,
                  double filled_qty,
                  double remaining_qty,
                  std::string_view status) {
    return json{
        {"type", "order"},
        {"ts", ts_ns},
        {"orderId", order_id},
        {"symbol", symbol},
        {"side", side == Side::Buy ? "BUY" : "SELL"},
        {"orderType", order_type},
        {"price", price},
        {"qty", qty},
        {"filledQty", filled_qty},
        {"remainingQty", remaining_qty},
        {"status", status},
    }
        .dump();
}

std::string toxicity(double bid_markout_5s,
                     double ask_markout_5s,
                     double bid_adverse_rate,
                     double ask_adverse_rate,
                     uint32_t bid_samples,
                     uint32_t ask_samples,
                     double bid_tox_score,
                     double ask_tox_score,
                     double bid_fill_rate,
                     double ask_fill_rate,
                     double bid_ttf_ms,
                     double ask_ttf_ms) {
    return json{
        {"type", "toxicity"},
        {"bidMarkout5s", bid_markout_5s},
        {"askMarkout5s", ask_markout_5s},
        {"bidAdverseRate", bid_adverse_rate},
        {"askAdverseRate", ask_adverse_rate},
        {"bidSamples", bid_samples},
        {"askSamples", ask_samples},
        {"bidToxScore", bid_tox_score},
        {"askToxScore", ask_tox_score},
        {"bidFillRate", bid_fill_rate},
        {"askFillRate", ask_fill_rate},
        {"bidTtfMs", bid_ttf_ms},
        {"askTtfMs", ask_ttf_ms},
    }
        .dump();
}

std::string market_color(uint64_t ts_ns,
                         std::string_view exchange,
                         std::string_view underlying,
                         const OptionsMarketColor& o,
                         const PerpMarketColor& p,
                         const FlowMarketColor& f,
                         const RegimeMarketColor& r) {
    json options = {
        {"frontExpiry", o.front_expiry_yyyymmdd},
        {"frontTimeToExpiryY", finite_or_null(o.front_time_to_expiry_y)},
        {"frontForwardPrice", finite_or_null(o.front_forward_price)},
        {"frontAtmIv", finite_or_null(o.front_atm_iv)},
        {"frontRr25d", finite_or_null(o.front_rr_25d)},
        {"frontSkewSlope", finite_or_null(o.front_skew_slope)},
        {"backExpiry", o.back_expiry_yyyymmdd},
        {"backTimeToExpiryY", finite_or_null(o.back_time_to_expiry_y)},
        {"backAtmIv", finite_or_null(o.back_atm_iv)},
        {"termSpread", finite_or_null(o.term_spread)},
        {"gex", finite_or_null(o.gex)},
        {"maxPainStrike", finite_or_null(o.max_pain_strike)},
        {"totalOi", finite_or_null(o.total_oi)},
        {"strikeCount", o.strike_count},
        {"expiryCount", o.expiry_count},
        {"strikesWithOi", o.strikes_with_oi},
    };
    json perp = {
        {"fundingRate8h", finite_or_null(p.funding_rate_8h)},
        // nextFundingTs of 0 → "not yet known" → encode as null so the
        // frontend can render "—" instead of "1970-01-01".
        {"nextFundingTs", p.next_funding_ts == 0 ? json(nullptr) : json(p.next_funding_ts)},
        {"basisBps", finite_or_null(p.basis_bps)},
        {"markPrice", finite_or_null(p.mark_price)},
        {"indexPrice", finite_or_null(p.index_price)},
    };
    json flow = {
        {"buyNotional5m", finite_or_null(f.buy_notional_5m)},
        {"sellNotional5m", finite_or_null(f.sell_notional_5m)},
        {"imbalance5m", finite_or_null(f.imbalance_5m)},
        {"tradeCount5m", f.trade_count_5m},
    };
    json regime = {
        {"realizedVol1h", finite_or_null(r.realized_vol_1h)},
        {"sampleCount", r.sample_count},
    };
    return json{
        {"type", "marketColor"},
        {"ts", ts_ns},
        {"exchange", exchange},
        {"underlying", underlying},
        {"options", std::move(options)},
        {"perp", std::move(perp)},
        {"flow", std::move(flow)},
        {"regime", std::move(regime)},
    }
        .dump();
}

}  // namespace bpt::bridge::encode
