#include "refdata/messaging/refdata_status_publisher.h"

#include "refdata/messaging/sbe_utils.h"

#include <messages/MessageHeader.h>
#include <messages/RefDataError.h>
#include <messages/RefDataReady.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::refdata::messaging {

RefdataStatusPublisher::RefdataStatusPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                             const std::string& channel,
                                             int stream_id) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
}

void RefdataStatusPublisher::publish_ready(uint8_t exchanges_loaded,
                                          uint16_t instrument_count,
                                          bool fee_schedules_loaded) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + RefDataReady::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

    RefDataReady msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .timestampNs(now_ns)
        .exchangesLoaded(exchanges_loaded)
        .instrumentCount(instrument_count)
        .feeSchedulesLoaded(fee_schedules_loaded ? uint8_t{1} : uint8_t{0})
        .fundingRatesLoaded(uint8_t{0});  // Funding rates moved to MdGateway; always 0 from Refdata

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), kBufSize);
    aeron_offer(*publication_, ab, static_cast<aeron::util::index_t>(kBufSize), "refdata_ready");

    bpt::common::log::info("RefDataReady published exchanges_loaded=0x{:02x} instruments={} fee_schedules={}",
                   exchanges_loaded,
                   instrument_count,
                   fee_schedules_loaded);
}

void RefdataStatusPublisher::publish_error(bpt::messages::RefDataErrorType::Value error_type,
                                          bpt::messages::ExchangeId::Value exchange_id,
                                          uint64_t instrument_id) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + RefDataError::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

    RefDataError msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .timestampNs(now_ns)
        .errorType(error_type)
        .exchangeId(exchange_id)
        .instrumentId(instrument_id);

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), kBufSize);
    aeron_offer(*publication_, ab, static_cast<aeron::util::index_t>(kBufSize), "refdata_error");

    bpt::common::log::error("RefDataError published error_type={} exchange={} instrument_id={}",
                    RefDataErrorType::c_str(error_type),
                    ExchangeId::c_str(exchange_id),
                    instrument_id);
}

}  // namespace bpt::refdata::messaging
