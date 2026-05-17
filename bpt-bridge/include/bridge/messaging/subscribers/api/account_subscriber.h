#pragma once

/// @file
/// Port: AccountSnapshot subscriber. Dispatches decoded balance / equity /
/// open-position state via the configured handler. Aeron concrete in
/// `aeron/account_subscriber.h`.

#include <cstdint>
#include <functional>
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

    using Handler = std::function<void(const Snapshot&)>;

    virtual ~AccountSubscriber() = default;

    void set_handler(Handler h) { handler_ = std::move(h); }

    virtual int poll(int fragment_limit = 8) = 0;

protected:
    Handler handler_;
};

}  // namespace bpt::bridge::messaging::api
