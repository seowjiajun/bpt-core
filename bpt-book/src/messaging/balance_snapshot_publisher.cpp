#include "book/messaging/balance_snapshot_publisher.h"

#include <messages/BalanceSnapshot.h>
#include <messages/MessageHeader.h>

#include <algorithm>
#include <bpt_common/logging.h>

namespace bpt::book::messaging {

using Policy = bpt::common::aeron::Publisher::Policy;

BalanceSnapshotPublisher::BalanceSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                                   const std::string& channel,
                                                   int stream_id)
    // BalanceSnapshot is idempotent — the next poll replaces stale
    // data — so kBoundedRetry on back-pressure + drop on no-subscriber
    // is the right shape.
    : publisher_(std::move(aeron), channel, stream_id, Policy::kBoundedRetry) {}

void BalanceSnapshotPublisher::publish(const adapter::BalanceSnapshot& snapshot) {
    using namespace bpt::messages;

    constexpr std::size_t kMaxRows = 256;
    constexpr std::size_t kRowSize = 1 + 8 + 8 + 8 + 8 + 8;
    constexpr std::size_t kBufSize =
        MessageHeader::encodedLength() + BalanceSnapshot::sbeBlockLength() + 4 + kMaxRows * kRowSize;

    char buf[kBufSize]{};
    const std::size_t n = std::min(snapshot.rows.size(), kMaxRows);

    BalanceSnapshot msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize).correlationId(snapshot.correlation_id).timestampNs(snapshot.timestamp_ns);

    auto& group = msg.balancesCount(static_cast<uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const auto& r = snapshot.rows[i];
        group.next()
            .exchangeId(r.exchange_id)
            .putSubAccount(r.sub_account)
            .putCcy(r.ccy)
            .totalE8(r.total_e8)
            .freeE8(r.free_e8)
            .holdE8(r.hold_e8);
    }

    const auto encoded_len = static_cast<::aeron::util::index_t>(MessageHeader::encodedLength() + msg.encodedLength());
    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), encoded_len);

    if (publisher_.offer(ab, 0, encoded_len))
        bpt::common::log::info("BalanceSnapshot published rows={}", n);
    else
        bpt::common::log::debug("BalanceSnapshot drop rows={} (no subscriber / back-pressure)", n);
}

}  // namespace bpt::book::messaging
