#include "refdata/messaging/fee_schedule_publisher.h"

#include "refdata/messaging/sbe_utils.h"

#include <messages/FeeSchedule.h>
#include <messages/MessageHeader.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::refdata::messaging {

FeeSchedulePublisher::FeeSchedulePublisher(std::shared_ptr<aeron::Aeron> aeron,
                                           const std::string& channel,
                                           int stream_id) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
}

void FeeSchedulePublisher::publish(const refdata::FeeScheduleState& fs) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + FeeSchedule::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    FeeSchedule msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .exchangeId(fs.exchange_id)
        .instrumentId(fs.instrument_id)
        .instrumentType(fs.instrument_type)
        .makerFeeBps(fs.maker_fee_bps)
        .takerFeeBps(fs.taker_fee_bps)
        .updatedTs(fs.updated_ts);

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), kBufSize);
    aeron_offer(*publication_, ab, static_cast<aeron::util::index_t>(kBufSize), "fee_schedule");

    bpt::common::log::debug("[Refdata] FeeSchedule published exchange={} instrument_id={} maker={}bps taker={}bps",
                    ExchangeId::c_str(fs.exchange_id),
                    fs.instrument_id,
                    fs.maker_fee_bps,
                    fs.taker_fee_bps);
}

}  // namespace bpt::refdata::messaging
