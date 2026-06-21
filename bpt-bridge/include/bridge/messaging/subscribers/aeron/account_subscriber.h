#pragma once

/// @file
/// Aeron-backed AccountSnapshot subscriber (OrderGateway stream 3004).
/// CRTP-templated on the Handler — calls H::on_account_snapshot(Snapshot&).

#include "bridge/aeron/sbe_decode.h"
#include "bridge/messaging/subscribers/api/account_subscriber.h"

#include <Aeron.h>

#include <messages/AccountSnapshot.h>

#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/aeron/subscriber.h>
#include <bpt_common/logging.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bpt::bridge::messaging::aeron {

template <class Handler>
class AccountSubscriber final : public api::AccountSubscriber {
public:
    AccountSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream) {
        sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            std::move(aeron),
            stream.channel,
            stream.stream_id,
            [this](::aeron::AtomicBuffer& b, ::aeron::util::index_t o, ::aeron::util::index_t l, ::aeron::Header& h) {
                on_fragment(b, o, l, h);
            });
        bpt::common::log::info("[bridge/Account] subscribed on {} stream {}", stream.channel, stream.stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 8) override { return sub_ ? sub_->poll(fragment_limit) : 0; }

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& /*header*/) {
        constexpr double kE8 = 1e8;
        decode_sbe_fragment<bpt::messages::AccountSnapshot>(
            buffer,
            offset,
            length,
            [this, kE8](bpt::messages::AccountSnapshot& msg) {
                if (handler_ == nullptr) [[unlikely]]
                    return;

                Snapshot s{};
                s.ts_ns = msg.timestampNs();
                s.exchange_id = static_cast<uint8_t>(msg.exchangeId());
                s.available_balance = static_cast<double>(msg.availableBalanceE8()) / kE8;
                s.total_equity = static_cast<double>(msg.totalEquityE8()) / kE8;
                if (s.total_equity == 0.0)
                    s.total_equity = s.available_balance;

                auto& positions = msg.positions();
                const std::size_t n = positions.count();
                s.positions.reserve(n);
                for (std::size_t i = 0; i < n; ++i) {
                    positions.next();
                    Position p;
                    p.exchange_symbol = positions.getExchangeSymbolAsString();
                    p.net_qty = static_cast<double>(positions.netQtyE8()) / kE8;
                    p.avg_entry = static_cast<double>(positions.avgEntryPriceE8()) / kE8;
                    p.unrealized_pnl = static_cast<double>(positions.unrealizedPnlE8()) / kE8;
                    if (p.net_qty == 0.0)
                        continue;
                    s.positions.push_back(std::move(p));
                }

                if (msg.currencyBalancesInActingVersion()) {
                    auto& ccy_group = msg.currencyBalances();
                    const std::size_t m = ccy_group.count();
                    s.currency_balances.reserve(m);
                    for (std::size_t i = 0; i < m; ++i) {
                        ccy_group.next();
                        CurrencyBalance cb;
                        cb.ccy = ccy_group.getCcyAsString();
                        cb.equity = static_cast<double>(ccy_group.equityE8()) / kE8;
                        cb.available_balance = static_cast<double>(ccy_group.availableBalanceE8()) / kE8;
                        if (cb.equity == 0.0 && cb.available_balance == 0.0)
                            continue;
                        s.currency_balances.push_back(std::move(cb));
                    }
                }

                handler_->on_account_snapshot(s);
            });
    }

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::bridge::messaging::aeron
