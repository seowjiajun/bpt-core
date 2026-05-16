#pragma once

/// @file
/// Subscribes to OrderGateway's AccountSnapshot stream (default 3004) and
/// delivers decoded balance / equity / open-position state from the live
/// exchange account.
///
/// The dashboard uses these as the canonical equity baseline so the
/// equity curve reflects the actual exchange balance rather than a
/// static `starting_capital` config value; positions feed the holdings
/// breakdown panel.

#include <Aeron.h>

#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bpt::bridge::messaging {

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

    AccountSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    void set_handler(Handler h) { handler_ = std::move(h); }

    int poll(int fragment_limit = 8);

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& header);

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    Handler handler_;
};

}  // namespace bpt::bridge::messaging
