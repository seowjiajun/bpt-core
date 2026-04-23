#pragma once

#include "book/adapter/balance_row.h"

namespace bpt::book::adapter {

// Per-venue balance adapter. bpt-book owns one instance per configured
// exchange, polls them round-robin, and publishes the aggregated rows.
// Implementations are expected to:
//  - open their own transport (HTTP client, WS, etc.) independent of
//    bpt-order-gateway's session; separate failure domains, separate
//    credentials (read-only where possible).
//  - throw on unrecoverable errors; bpt-book will log and retry next tick.
//  - emit one BalanceRow per (sub_account, ccy) tuple that has non-zero
//    balance. Zero rows are allowed but add wire noise; prefer to skip.
class IBalanceAdapter {
public:
    virtual ~IBalanceAdapter() = default;
    virtual const char* venue_name() const = 0;
    virtual std::vector<BalanceRow> fetch() = 0;
};

}  // namespace bpt::book::adapter
