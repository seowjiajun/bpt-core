#include "bridge/messaging/subscribers/account_subscriber.h"

#include "bridge/aeron/sbe_decode.h"

#include <messages/AccountSnapshot.h>

#include <bpt_common/logging.h>

namespace bpt::bridge::messaging {

namespace {
constexpr double kE8 = 1e8;
}

AccountSubscriber::AccountSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                     const std::string& channel,
                                     int32_t stream_id) {
    sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
        std::move(aeron),
        channel,
        stream_id,
        [this](::aeron::AtomicBuffer& b, ::aeron::util::index_t o, ::aeron::util::index_t l, ::aeron::Header& h) {
            on_fragment(b, o, l, h);
        });
    bpt::common::log::info("[bridge/Account] subscribed on {} stream {}", channel, stream_id);
}

int AccountSubscriber::poll(int fragment_limit) {
    return sub_ ? sub_->poll(fragment_limit) : 0;
}

void AccountSubscriber::on_fragment(::aeron::AtomicBuffer& buffer,
                                    ::aeron::util::index_t offset,
                                    ::aeron::util::index_t length,
                                    ::aeron::Header& /*header*/) {
    decode_sbe_fragment<bpt::messages::AccountSnapshot>(
        buffer,
        offset,
        length,
        [this](bpt::messages::AccountSnapshot& msg) {
            Snapshot s{};
            s.ts_ns = msg.timestampNs();
            s.exchange_id = static_cast<uint8_t>(msg.exchangeId());
            s.available_balance = static_cast<double>(msg.availableBalanceE8()) / kE8;
            s.total_equity = static_cast<double>(msg.totalEquityE8()) / kE8;
            if (s.total_equity == 0.0)
                s.total_equity = s.available_balance;

            // SBE group iterators are stateful — calling .next() advances the
            // read cursor, so the order we call it matters.
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

            // currencyBalances is gated on acting version ≥ 13.
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

            if (handler_)
                handler_(s);
        });
}

}  // namespace bpt::bridge::messaging
