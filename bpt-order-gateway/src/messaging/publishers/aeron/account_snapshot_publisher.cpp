#include "order_gateway/messaging/publishers/aeron/account_snapshot_publisher.h"

#include <messages/ExchangeId.h>

#include <bpt_common/logging.h>
#include <cstddef>

namespace bpt::order_gateway::messaging::aeron {

using Policy = bpt::common::aeron::Publisher::Policy;

AccountSnapshotPublisher::AccountSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                                   const bpt::common::config::StreamConfig& stream)
    : publisher_(std::move(aeron), stream.channel, stream.stream_id, Policy::kRetryOnBackpressure) {}

void AccountSnapshotPublisher::publish(const adapter::AccountSnapshotData& snapshot) {
    alignas(8) std::byte scratch[SbeAccountSnapshotCodec::kRecommendedScratchSize];
    const auto bytes = codec_.encode(snapshot, scratch);

    const auto len = static_cast<::aeron::util::index_t>(bytes.size());
    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), len);

    if (publisher_.offer(ab, 0, len))
        bpt::common::log::info("AccountSnapshot published exchange={} balance={:.2f} positions={} ccyBalances={}",
                               bpt::messages::ExchangeId::c_str(snapshot.exchange_id),
                               static_cast<double>(snapshot.available_balance_e8) / 1e8,
                               snapshot.positions.size(),
                               snapshot.currency_balances.size());
    else
        bpt::common::log::warn("AccountSnapshot dropped — no subscriber on {}:{}",
                               publisher_.channel(),
                               publisher_.stream_id());
}

}  // namespace bpt::order_gateway::messaging::aeron
