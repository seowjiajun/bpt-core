#include "bridge/message_encoder.h"

#include <nlohmann/json.hpp>

namespace bridge::encode {

using nlohmann::json;

std::string session(std::string_view symbol,
                    std::string_view strategy,
                    std::string_view exchange,
                    std::string_view mode,
                    double starting_capital) {
    return json{
        {"type", "session"},
        {"symbol", symbol},
        {"strategy", strategy},
        {"exchange", exchange},
        {"mode", mode},
        {"startingCapital", starting_capital},
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
                 double qty,
                 double price,
                 double realized_pnl,
                 double equity) {
    return json{
        {"type", "fill"},
        {"ts", ts_ns},
        {"orderId", order_id},
        {"symbol", symbol},
        {"side", side == Side::Buy ? "BUY" : "SELL"},
        {"qty", qty},
        {"price", price},
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

}  // namespace bridge::encode
