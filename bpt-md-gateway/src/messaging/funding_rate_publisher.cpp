#include "md_gateway/messaging/funding_rate_publisher.h"

#include <messages/FundingRate.h>
#include <messages/MessageHeader.h>

#include <thread>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::md_gateway::messaging {

FundingRatePublisher::FundingRatePublisher(std::shared_ptr<aeron::Aeron> aeron,
                                           const std::string& channel,
                                           int stream_id) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
}

void FundingRatePublisher::publish(const FundingRateUpdate& fr) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + FundingRate::sbeBlockLength();
    char buf[kBufSize] = {};

    FundingRate msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(fr.exchange_id)
        .instrumentId(fr.instrument_id)
        .rateBps(fr.rate_bps)
        .nextFundingTs(fr.next_funding_ts_ns)
        .collectedTs(fr.collected_ts_ns);

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), kBufSize);
    std::int64_t result;
    do {
        result = publication_->offer(ab, 0, static_cast<aeron::util::index_t>(kBufSize));
        if (result == aeron::NOT_CONNECTED || result == aeron::PUBLICATION_CLOSED)
            break;
        if (result < 0)
            std::this_thread::yield();
    } while (result < 0);

    bpt::common::log::debug("FundingRate published exchange={} instrument_id={} rate={}bps",
                    ExchangeId::c_str(fr.exchange_id),
                    fr.instrument_id,
                    fr.rate_bps);
}

}  // namespace bpt::md_gateway::messaging
