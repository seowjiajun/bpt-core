#pragma once

/// \file
/// \brief AccountSnapshotData — the value type returned by `IOrderAdapter::fetch_account_snapshot`.

#include <messages/ExchangeId.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bpt::order_gateway::adapter {

struct AccountPosition {
    std::string exchange_symbol;    ///< exchange-native symbol e.g. "BTCUSDT", "BTC-USDT-SWAP", "BTC"
    int64_t net_qty_e8{0};          ///< signed net position * 1e8 (long=+, short=-)
    int64_t avg_entry_price_e8{0};  ///< VWAP entry price * 1e8; 0 if flat
    int64_t unrealized_pnl_e8{0};   ///< unrealised PnL in USDT * 1e8
};

/// \brief Per-currency cash balance row.
///
/// OKX's `/account/balance` carries a `details[]` array with one entry
/// per currency the account holds. The single aggregated
/// `available_balance_e8` collapses this into USDT only; this struct
/// preserves the per-ccy breakdown so the dashboard can show a row per
/// currency (USDT, USDC, SGD, USD, BTC, etc.).
struct CurrencyBalance {
    std::string ccy;                  ///< currency code, e.g. "USDT", "USDC", "SGD" (≤ 8 chars)
    int64_t equity_e8{0};             ///< currency equity * 1e8 (what you hold in that ccy)
    int64_t available_balance_e8{0};  ///< withdrawable amount in that ccy * 1e8
};

struct AccountSnapshotData {
    bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::ALL};
    uint64_t correlation_id{0};
    uint64_t timestamp_ns{0};
    int64_t available_balance_e8{0};  ///< available margin/cash in USDT * 1e8
    int64_t total_equity_e8{0};       ///< total equity (incl. unrealised PnL) in USDT * 1e8
    std::vector<AccountPosition> positions;
    std::vector<CurrencyBalance> currency_balances;  ///< per-ccy cash rows (OKX: from details[])
};

}  // namespace bpt::order_gateway::adapter
