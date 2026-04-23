#pragma once

#include <messages/ExchangeId.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bpt::book::adapter {

// One balance row: (venue, sub_account, ccy) → total / free / hold.
// Mirrors the SBE BalanceSnapshot.balances group 1:1; adapters populate
// a std::vector of these and the publisher encodes them as-is.
struct BalanceRow {
    bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::ALL};
    std::string sub_account;   // ≤ 8 chars — "perps", "spot", "trading", "funding", "main"
    std::string ccy;           // ≤ 8 chars — "USDC", "USDT", "BTC", ...
    int64_t total_e8{0};       // total balance in ccy * 1e8
    int64_t free_e8{0};        // free (withdrawable) portion * 1e8
    int64_t hold_e8{0};        // portion held by open orders/positions * 1e8
};

struct BalanceSnapshot {
    uint64_t correlation_id{0};
    uint64_t timestamp_ns{0};
    std::vector<BalanceRow> rows;
};

}  // namespace bpt::book::adapter
