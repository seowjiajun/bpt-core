#pragma once

/// @file
/// Port: AccountSnapshot subscriber. CRTP-templated concrete in
/// `aeron::AccountSubscriber<H>` calls H::on_account_snapshot.

#include <cstdint>
#include <string>
#include <vector>

namespace bpt::bridge::messaging::api {

class AccountSubscriber {
public:
    struct Position {
        std::string exchange_symbol;
        double net_qty;
        double avg_entry;
        double unrealized_pnl;
    };

    struct CurrencyBalance {
        std::string ccy;
        double equity;
        double available_balance;
    };

    struct Snapshot {
        uint64_t ts_ns;
        uint8_t exchange_id;
        double available_balance;
        double total_equity;
        std::vector<Position> positions;
        std::vector<CurrencyBalance> currency_balances;
    };

    virtual ~AccountSubscriber() = default;

    virtual int poll(int fragment_limit = 8) = 0;
};

}  // namespace bpt::bridge::messaging::api
