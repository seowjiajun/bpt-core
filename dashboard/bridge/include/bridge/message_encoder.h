#pragma once

// Encodes bridge → frontend JSON messages.  The shapes MUST match the
// TypeScript schema in dashboard/frontend/src/types/messages.ts.

#include <cstdint>
#include <string>
#include <vector>

namespace bridge::encode {

enum class Side : uint8_t { Buy, Sell };

// { "type":"session", "symbol":"...", "strategy":"...", "exchange":"...",
//   "mode":"paper|live", "instrumentType":"SPOT|PERP|FUTURE|OPTION" }
//
// Equity baseline is sourced from order-gateway AccountSnapshots ("account"
// messages), not from a static config value. The dashboard derives the
// equity curve and risk metrics from those snapshots.
std::string session(std::string_view symbol,
                    std::string_view strategy,
                    std::string_view exchange,
                    std::string_view mode,
                    std::string_view instrument_type);

// { "type":"status", "state":"live" }   // "live" | "mock" | "halted" | "off"
std::string status(std::string_view state);

// { "type":"tick", "ts":..., "symbol":"BTC-USDT", "price":... }
std::string tick(uint64_t ts_ns, std::string_view symbol, double price);

// { "type":"fill", "ts":..., "orderId":..., "symbol":"...", "side":"BUY",
//   "orderType":"LIMIT", "qty":..., "price":..., "fee":..., ... }
std::string fill(uint64_t ts_ns,
                 uint64_t order_id,
                 std::string_view symbol,
                 Side side,
                 std::string_view order_type,
                 double qty,
                 double price,
                 double fee,
                 double realized_pnl,
                 double equity);

// { "type":"position", "symbol":"...", "netQty":..., "avgEntry":..., "unrealizedPnl":... }
std::string position(std::string_view symbol,
                     double net_qty,
                     double avg_entry,
                     double unrealized_pnl);

// Per-position row in an `account` message. Mirrors the `Position`
// SBE repeating group inside order-gateway's AccountSnapshot.
struct AccountPosition {
    std::string symbol;         // exchange-native symbol (e.g. "BTC")
    double      net_qty;        // signed (+ long, − short) in coin units
    double      avg_entry;      // quote currency
    double      unrealized_pnl; // quote currency
};

// Per-currency cash row. Mirrors the `CurrencyBalances` SBE group.
struct AccountCurrencyBalance {
    std::string ccy;                // currency code, ≤ 8 chars
    double      equity;             // per-currency equity (natural units)
    double      available_balance;  // per-currency withdrawable amount
};

// { "type":"account", "ts":..., "balance":..., "equity":...,
//   "positions":[...],
//   "currencyBalances":[{"ccy":"USDT", "equity":..., "availableBalance":...}, ...] }
//
// Live exchange account snapshot from order-gateway — the canonical equity
// baseline for the dashboard. `positions` feeds crypto rows in the
// holdings panel; `currencyBalances` feeds per-stable-ccy rows.
std::string account(uint64_t ts_ns,
                    double balance,
                    double equity,
                    const std::vector<AccountPosition>& positions,
                    const std::vector<AccountCurrencyBalance>& currency_balances);

// { "type":"order", "ts":..., "orderId":..., "symbol":"...", "side":"BUY",
//   "orderType":"LIMIT", "price":..., "qty":..., "filledQty":...,
//   "remainingQty":..., "status":"acked" }
std::string order(uint64_t ts_ns,
                  uint64_t order_id,
                  std::string_view symbol,
                  Side side,
                  std::string_view order_type,
                  double price,
                  double qty,
                  double filled_qty,
                  double remaining_qty,
                  std::string_view status);

// { "type":"toxicity", "bidMarkout5s":..., "askMarkout5s":...,
//   "bidAdverseRate":..., "askAdverseRate":...,
//   "bidSamples":..., "askSamples":...,
//   "bidToxScore":..., "askToxScore":... }
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
                     double ask_ttf_ms);

}  // namespace bridge::encode
