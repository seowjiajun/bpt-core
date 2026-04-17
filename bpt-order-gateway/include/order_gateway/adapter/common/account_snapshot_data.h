#pragma once

#include <messages/ExchangeId.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bpt::order_gateway::adapter {

struct AccountPosition {
    std::string exchange_symbol;    // exchange-native symbol e.g. "BTCUSDT", "BTC-USDT-SWAP", "BTC"
    int64_t net_qty_e8{0};          // signed net position * 1e8 (long=+, short=-)
    int64_t avg_entry_price_e8{0};  // VWAP entry price * 1e8; 0 if flat
    int64_t unrealized_pnl_e8{0};   // unrealised PnL in USDT * 1e8
};

struct AccountSnapshotData {
    bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::ALL};
    uint64_t correlation_id{0};
    uint64_t timestamp_ns{0};
    int64_t available_balance_e8{0};  // available margin/cash in USDT * 1e8
    int64_t total_equity_e8{0};       // total equity (incl. unrealised PnL) in USDT * 1e8
    std::vector<AccountPosition> positions;
};

}  // namespace bpt::order_gateway::adapter
