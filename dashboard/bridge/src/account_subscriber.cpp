#include "bridge/account_subscriber.h"

#include <messages/AccountSnapshot.h>
#include <messages/MessageHeader.h>

#include <chrono>
#include <thread>
#include <bpt_common/logging.h>

namespace bridge {

namespace {
constexpr double kE8 = 1e8;
}

AccountSubscriber::AccountSubscriber(std::shared_ptr<aeron::Aeron> aeron,
                                     const std::string& channel,
                                     int32_t stream_id) {
    const int64_t reg_id = aeron->addSubscription(channel, stream_id);
    for (int i = 0; i < 500; ++i) {
        sub_ = aeron->findSubscription(reg_id);
        if (sub_) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!sub_) {
        bpt::common::log::error("[bridge/Account] failed to register subscription on {} stream {}",
                        channel, stream_id);
    } else {
        bpt::common::log::info("[bridge/Account] subscribed on {} stream {}", channel, stream_id);
    }
}

int AccountSubscriber::poll(int fragment_limit) {
    if (!sub_) return 0;
    return sub_->poll(
        [this](const aeron::concurrent::AtomicBuffer& b,
               aeron::util::index_t o,
               aeron::util::index_t l,
               const aeron::Header& h) { on_fragment(b, o, l, h); },
        fragment_limit);
}

void AccountSubscriber::on_fragment(const aeron::concurrent::AtomicBuffer& buffer,
                                    aeron::util::index_t offset,
                                    aeron::util::index_t length,
                                    const aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength())) return;

    char* data = const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset));
    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

    if (hdr.templateId() != AccountSnapshot::sbeTemplateId()) return;

    AccountSnapshot msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<uint64_t>(length));

    Snapshot s{};
    s.ts_ns             = msg.timestampNs();
    s.exchange_id       = static_cast<uint8_t>(msg.exchangeId());
    s.available_balance = static_cast<double>(msg.availableBalanceE8()) / kE8;
    s.total_equity      = static_cast<double>(msg.totalEquityE8()) / kE8;
    if (s.total_equity == 0.0) s.total_equity = s.available_balance;

    // Decode the positions repeating group. SBE group iterators are
    // stateful — calling .next() advances the read cursor, so the order
    // we call it matters. Empty positions (net_qty == 0) are skipped by
    // the publisher on order-gateway's side but we also filter defensively
    // in case a dead leg shows up.
    auto& positions = msg.positions();
    const std::size_t n = positions.count();
    s.positions.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        positions.next();
        Position p;
        p.exchange_symbol = positions.getExchangeSymbolAsString();
        p.net_qty         = static_cast<double>(positions.netQtyE8())        / kE8;
        p.avg_entry       = static_cast<double>(positions.avgEntryPriceE8()) / kE8;
        p.unrealized_pnl  = static_cast<double>(positions.unrealizedPnlE8()) / kE8;
        if (p.net_qty == 0.0) continue;
        s.positions.push_back(std::move(p));
    }

    // SBE repeating groups share a read cursor with the parent message;
    // positions must be fully iterated above before accessing the next
    // group. currencyBalances is gated on acting version ≥ 13 — older
    // serialized snapshots decode cleanly with an empty group.
    if (msg.currencyBalancesInActingVersion()) {
        auto& ccy_group = msg.currencyBalances();
        const std::size_t m = ccy_group.count();
        s.currency_balances.reserve(m);
        for (std::size_t i = 0; i < m; ++i) {
            ccy_group.next();
            CurrencyBalance cb;
            cb.ccy               = ccy_group.getCcyAsString();
            cb.equity            = static_cast<double>(ccy_group.equityE8())            / kE8;
            cb.available_balance = static_cast<double>(ccy_group.availableBalanceE8()) / kE8;
            if (cb.equity == 0.0 && cb.available_balance == 0.0) continue;
            s.currency_balances.push_back(std::move(cb));
        }
    }

    if (handler_) handler_(s);
}

}  // namespace bridge
