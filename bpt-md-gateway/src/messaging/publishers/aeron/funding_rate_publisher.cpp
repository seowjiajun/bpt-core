#include "md_gateway/messaging/publishers/aeron/funding_rate_publisher.h"

#include <messages/ExchangeId.h>

#include <bpt_common/logging.h>
#include <cstddef>

namespace bpt::md_gateway::messaging::aeron {

using Policy = bpt::common::aeron::Publisher::Policy;

FundingRatePublisher::FundingRatePublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                           const bpt::common::config::StreamConfig& stream)
    // Original code had custom "spin on back-pressure, exit on
    // NOT_CONNECTED/CLOSED" — now the default kRetryOnBackpressure
    // policy is exactly that.
    : publisher_(std::move(aeron), stream.channel, stream.stream_id, Policy::kRetryOnBackpressure) {}

void FundingRatePublisher::publish(const FundingRateUpdate& fr) {
    alignas(8) std::byte scratch[SbeFundingRateCodec::kRecommendedScratchSize];
    const auto bytes = codec_.encode(fr, scratch);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(bytes.size()));

    bpt::common::log::debug("FundingRate published exchange={} instrument_id={} rate={}bps",
                            bpt::messages::ExchangeId::c_str(fr.exchange_id),
                            fr.instrument_id,
                            fr.rate_bps);
}

}  // namespace bpt::md_gateway::messaging::aeron
