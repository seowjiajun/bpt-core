#include "order_gateway/messaging/account_snapshot_publisher.h"

#include <messages/AccountSnapshot.h>
#include <messages/MessageHeader.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>

namespace bpt::order_gateway::messaging {

AccountSnapshotPublisher::AccountSnapshotPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                                   const std::string& channel,
                                                   int stream_id) {
    publication_ = ygg::aeron::wait_for_publication(aeron, channel, stream_id);
}

void AccountSnapshotPublisher::publish(const adapter::AccountSnapshotData& snapshot) {
    using namespace bpt::messages;

    // AccountSnapshot has a variable-length positions group.
    // Max 500 positions; each entry: ExchangeSymbol(32) + 3×int64(24) = 56 bytes.
    constexpr std::size_t kMaxPositions = 500;
    constexpr std::size_t kPositionSize = 32 + 8 + 8 + 8;  // per SBE block
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + AccountSnapshot::sbeBlockLength() +
                                     4 +  // groupSizeEncoding
                                     kMaxPositions * kPositionSize;

    char buf[kBufSize]{};

    const std::size_t n = std::min(snapshot.positions.size(), kMaxPositions);

    AccountSnapshot msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(snapshot.exchange_id)
        .correlationId(snapshot.correlation_id)
        .timestampNs(snapshot.timestamp_ns)
        .availableBalanceE8(snapshot.available_balance_e8)
        .totalEquityE8(snapshot.total_equity_e8);

    auto& group = msg.positionsCount(static_cast<uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const auto& pos = snapshot.positions[i];
        group.next()
            .putExchangeSymbol(pos.exchange_symbol)
            .netQtyE8(pos.net_qty_e8)
            .avgEntryPriceE8(pos.avg_entry_price_e8)
            .unrealizedPnlE8(pos.unrealized_pnl_e8);
    }

    const auto encoded_len = static_cast<aeron::util::index_t>(MessageHeader::encodedLength() + msg.encodedLength());
    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), encoded_len);

    std::lock_guard lock(mutex_);
    while (publication_->offer(ab, 0, encoded_len) < 0)
        std::this_thread::yield();

    ygg::log::info("[Heimdall] AccountSnapshot published exchange={} balance={:.2f} positions={}",
                   ExchangeId::c_str(snapshot.exchange_id),
                   static_cast<double>(snapshot.available_balance_e8) / 1e8,
                   n);
}

}  // namespace bpt::order_gateway::messaging
