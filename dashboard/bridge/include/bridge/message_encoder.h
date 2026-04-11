#pragma once

// Encodes bridge → frontend JSON messages.  The shapes MUST match the
// TypeScript schema in dashboard/frontend/src/types/messages.ts.

#include <cstdint>
#include <string>

namespace bridge::encode {

enum class Side : uint8_t { Buy, Sell };

// { "type":"session", "symbol":"...", "strategy":"...", "exchange":"...", "startingCapital":100000 }
std::string session(std::string_view symbol,
                    std::string_view strategy,
                    std::string_view exchange,
                    double starting_capital);

// { "type":"status", "state":"live" }   // "live" | "mock" | "halted" | "off"
std::string status(std::string_view state);

// { "type":"tick", "ts":..., "symbol":"BTC-USDT", "price":... }
std::string tick(uint64_t ts_ns, std::string_view symbol, double price);

// { "type":"fill", "ts":..., "orderId":..., "symbol":"...", "side":"BUY", ... }
std::string fill(uint64_t ts_ns,
                 uint64_t order_id,
                 std::string_view symbol,
                 Side side,
                 double qty,
                 double price,
                 double realized_pnl,
                 double equity);

// { "type":"position", "symbol":"...", "netQty":..., "avgEntry":..., "unrealizedPnl":... }
std::string position(std::string_view symbol,
                     double net_qty,
                     double avg_entry,
                     double unrealized_pnl);

}  // namespace bridge::encode
