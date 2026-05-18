#include "refdata/messaging/publishers/aeron/fee_schedule_publisher.h"

#include "refdata/messaging/sbe_utils.h"

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <cstddef>
#include <messages/ExchangeId.h>

namespace bpt::refdata::messaging::aeron {

FeeSchedulePublisher::FeeSchedulePublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                           const std::string& channel,
                                           int stream_id) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
}

void FeeSchedulePublisher::publish(const model::FeeScheduleState& fs) {
    alignas(8) std::byte scratch[SbeFeeScheduleCodec::kRecommendedScratchSize];
    const auto bytes = codec_.encode(fs, scratch);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(scratch), static_cast<::aeron::util::index_t>(bytes.size()));
    aeron_offer(*publication_, ab, static_cast<::aeron::util::index_t>(bytes.size()), "fee_schedule");

    bpt::common::log::debug("FeeSchedule published exchange={} instrument_id={} maker={}bps taker={}bps",
                            bpt::messages::ExchangeId::c_str(fs.exchange_id),
                            fs.instrument_id,
                            fs.maker_fee_bps,
                            fs.taker_fee_bps);
}

}  // namespace bpt::refdata::messaging::aeron
