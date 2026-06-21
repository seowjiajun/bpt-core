#include "md_gateway/messaging/publishers/aeron/instrument_stats_publisher.h"

#include <messages/ExchangeId.h>

#include <bpt_common/logging.h>
#include <cstddef>

namespace bpt::md_gateway::messaging::aeron {

using Policy = bpt::common::aeron::Publisher::Policy;

InstrumentStatsPublisher::InstrumentStatsPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                                   const bpt::common::config::StreamConfig& stream)
    : publisher_(std::move(aeron), stream.channel, stream.stream_id, Policy::kRetryOnBackpressure) {}

void InstrumentStatsPublisher::publish(const InstrumentStatsUpdate& stats) {
    alignas(8) std::byte scratch[SbeInstrumentStatsCodec::kRecommendedScratchSize];
    const auto bytes = codec_.encode(stats, scratch);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(bytes.size()));

    bpt::common::log::debug("InstrumentStats published exchange={} instrument_id={} oi={} vol24h={} mark={}",
                            bpt::messages::ExchangeId::c_str(stats.exchange_id),
                            stats.instrument_id,
                            stats.open_interest,
                            stats.volume_24h,
                            stats.mark_price);
}

}  // namespace bpt::md_gateway::messaging::aeron
