#include "book/messaging/balance_snapshot_publisher.h"

#include <messages/BalanceSnapshot.h>
#include <messages/MessageHeader.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::book::messaging {

BalanceSnapshotPublisher::BalanceSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                                   const std::string& channel,
                                                   int stream_id) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
}

void BalanceSnapshotPublisher::publish(const adapter::BalanceSnapshot& snapshot) {
    using namespace bpt::messages;

    // Per-row size: exchangeId(1) + subAccount(8) + ccy(8) + 3×int64 = 41 bytes
    // + 4-byte group header. Cap at 256 rows — 4 venues × many ccys is still
    // comfortably under this.
    constexpr std::size_t kMaxRows = 256;
    constexpr std::size_t kRowSize = 1 + 8 + 8 + 8 + 8 + 8;
    constexpr std::size_t kBufSize =
        MessageHeader::encodedLength() + BalanceSnapshot::sbeBlockLength() +
        4 + kMaxRows * kRowSize;

    char buf[kBufSize]{};
    const std::size_t n = std::min(snapshot.rows.size(), kMaxRows);

    BalanceSnapshot msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .correlationId(snapshot.correlation_id)
        .timestampNs(snapshot.timestamp_ns);

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

    const auto encoded_len =
        static_cast<::aeron::util::index_t>(MessageHeader::encodedLength() + msg.encodedLength());
    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), encoded_len);

    std::lock_guard lock(mutex_);
    // Retry on BACK_PRESSURED / ADMIN_ACTION; drop on NOT_CONNECTED.
    // BalanceSnapshot is idempotent — the next poll republishes, so
    // missing a publish when no consumer is listening is fine (and far
    // better than spinning the poll thread forever).
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const auto rc = publication_->offer(ab, 0, encoded_len);
        if (rc >= 0) {
            bpt::common::log::info("BalanceSnapshot published rows={}", n);
            return;
        }
        if (rc == ::aeron::NOT_CONNECTED) {
            bpt::common::log::info("BalanceSnapshot drop — no subscriber on stream (rows={})", n);
            return;
        }
        std::this_thread::yield();
    }
    bpt::common::log::warn("BalanceSnapshot publish timed out after 1000 attempts (rows={})", n);
}

}  // namespace bpt::book::messaging
