#include "md_gateway/messaging/publishers/instrument_stats_publisher.h"

#include <messages/InstrumentStats.h>
#include <messages/MessageHeader.h>

#include <bpt_common/logging.h>

namespace bpt::md_gateway::messaging {

using Policy = bpt::common::aeron::Publisher::Policy;

InstrumentStatsPublisher::InstrumentStatsPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                                   const std::string& channel,
                                                   int stream_id)
    : publisher_(std::move(aeron), channel, stream_id, Policy::kRetryOnBackpressure) {}

void InstrumentStatsPublisher::publish(const InstrumentStatsUpdate& stats) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + InstrumentStats::sbeBlockLength();
    char buf[kBufSize] = {};

    InstrumentStats msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(stats.exchange_id)
        .instrumentId(stats.instrument_id)
        .openInterest(stats.open_interest)
        .volume24h(stats.volume_24h)
        .markPrice(stats.mark_price)
        .indexPrice(stats.index_price)
        .lastPrice(stats.last_price)
        .collectedTs(stats.collected_ts_ns);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), kBufSize);
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(kBufSize));

    bpt::common::log::debug("InstrumentStats published exchange={} instrument_id={} oi={} vol24h={} mark={}",
                            ExchangeId::c_str(stats.exchange_id),
                            stats.instrument_id,
                            stats.open_interest,
                            stats.volume_24h,
                            stats.mark_price);
}

}  // namespace bpt::md_gateway::messaging
