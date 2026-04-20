#include "order_gateway/messaging/account_snapshot_publisher.h"

#include <messages/AccountSnapshot.h>
#include <messages/MessageHeader.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::order_gateway::messaging {

AccountSnapshotPublisher::AccountSnapshotPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                                   const std::string& channel,
                                                   int stream_id) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
}

void AccountSnapshotPublisher::publish(const adapter::AccountSnapshotData& snapshot) {
    using namespace bpt::messages;

    // AccountSnapshot has two variable-length groups: positions + currency
    // balances. Buffer sizing is a worst-case ceiling.
    //   positions:    up to 500 × 56 bytes (32-char symbol + 3×int64) + 4-byte header
    //   currencyBal:  up to 32  × 24 bytes (8-char ccy   + 2×int64) + 4-byte header
    constexpr std::size_t kMaxPositions = 500;
    constexpr std::size_t kPositionSize = 32 + 8 + 8 + 8;  // per SBE block
    constexpr std::size_t kMaxCurrencyBalances = 32;
    constexpr std::size_t kCurrencyBalanceSize = 8 + 8 + 8;  // per SBE block
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + AccountSnapshot::sbeBlockLength() +
                                     4 + kMaxPositions * kPositionSize +
                                     4 + kMaxCurrencyBalances * kCurrencyBalanceSize;

    char buf[kBufSize]{};

    const std::size_t n_pos = std::min(snapshot.positions.size(), kMaxPositions);
    const std::size_t n_ccy = std::min(snapshot.currency_balances.size(), kMaxCurrencyBalances);

    AccountSnapshot msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(snapshot.exchange_id)
        .correlationId(snapshot.correlation_id)
        .timestampNs(snapshot.timestamp_ns)
        .availableBalanceE8(snapshot.available_balance_e8)
        .totalEquityE8(snapshot.total_equity_e8);

    auto& pos_group = msg.positionsCount(static_cast<uint16_t>(n_pos));
    for (std::size_t i = 0; i < n_pos; ++i) {
        const auto& pos = snapshot.positions[i];
        pos_group.next()
            .putExchangeSymbol(pos.exchange_symbol)
            .netQtyE8(pos.net_qty_e8)
            .avgEntryPriceE8(pos.avg_entry_price_e8)
            .unrealizedPnlE8(pos.unrealized_pnl_e8);
    }

    auto& ccy_group = msg.currencyBalancesCount(static_cast<uint16_t>(n_ccy));
    for (std::size_t i = 0; i < n_ccy; ++i) {
        const auto& cb = snapshot.currency_balances[i];
        ccy_group.next()
            .putCcy(cb.ccy)
            .equityE8(cb.equity_e8)
            .availableBalanceE8(cb.available_balance_e8);
    }

    const auto encoded_len = static_cast<aeron::util::index_t>(MessageHeader::encodedLength() + msg.encodedLength());
    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), encoded_len);

    std::lock_guard lock(mutex_);
    while (publication_->offer(ab, 0, encoded_len) < 0)
        std::this_thread::yield();

    bpt::common::log::info("AccountSnapshot published exchange={} balance={:.2f} positions={} ccyBalances={}",
                   ExchangeId::c_str(snapshot.exchange_id),
                   static_cast<double>(snapshot.available_balance_e8) / 1e8,
                   n_pos,
                   n_ccy);
}

}  // namespace bpt::order_gateway::messaging
