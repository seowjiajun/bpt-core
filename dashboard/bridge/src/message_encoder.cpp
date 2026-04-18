#include "bridge/message_encoder.h"

#include <nlohmann/json.hpp>

namespace bridge::encode {

using nlohmann::json;

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
    }.dump();
}

std::string status(std::string_view state) {
    return json{
        {"type", "status"},
        {"state", state},
    }.dump();
}

std::string tick(uint64_t ts_ns, std::string_view symbol, double price) {
    return json{
        {"type", "tick"},
        {"ts", ts_ns},
        {"symbol", symbol},
        {"price", price},
    }.dump();
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
    }.dump();
}

std::string position(std::string_view symbol,
                     double net_qty,
                     double avg_entry,
                     double unrealized_pnl) {
    return json{
        {"type", "position"},
        {"symbol", symbol},
        {"netQty", net_qty},
        {"avgEntry", avg_entry},
        {"unrealizedPnl", unrealized_pnl},
    }.dump();
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
    }.dump();
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
    }.dump();
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
    }.dump();
}

}  // namespace bridge::encode
