#include "refdata/messaging/publishers/refdata_delta_publisher.h"

#include "refdata/messaging/sbe_utils.h"

#include <messages/MessageHeader.h>
#include <messages/OptionSide.h>
#include <messages/RefDataDelta.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::refdata::messaging {

RefdataDeltaPublisher::RefdataDeltaPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                             const std::string& channel,
                                             int stream_id) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
}

void RefdataDeltaPublisher::publish_delta(bpt::messages::DeltaUpdateType::Value update_type,
                                          const refdata::Instrument& inst) {
    ++seq_;

    using namespace bpt::messages;

    // Fixed-size buffer: SBE header (8) + RefDataDelta block (136)
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + RefDataDelta::sbeBlockLength();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[kBufSize];

    uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

    RefDataDelta msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .deltaSeqNum(seq_)
        .timestampNs(now_ns)
        .updateType(update_type)
        .instrumentId(inst.inst_uid);

    put_str<24>(msg.symbol(), inst.venue_symbol);
    put_str<8>(msg.exchange(), inst.venue);
    put_str<8>(msg.baseCurrency(), inst.base);
    put_str<8>(msg.quoteCurrency(), inst.quote);

    msg.instrumentType(to_sbe_type(inst.inst_type))
        .status(to_sbe_status(inst.status))
        .lotSize(inst.lot_size)
        .tickSize(inst.tick_size)
        .contractSize(inst.contract_multiplier)
        .expiryDate(inst.expiry_timestamp.has_value() ? ns_to_yyyymmdd(*inst.expiry_timestamp) : 0u)
        .optionSide(OptionSide::NA)
        .strikePrice(inst.strike_price.value_or(0.0));
    put_str<24>(msg.underlying(), inst.base);

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), kBufSize);
    aeron_offer(*publication_, ab, static_cast<aeron::util::index_t>(kBufSize), "delta");

    bpt::common::log::debug("Published delta seq={} uid={} type={}",
                            seq_,
                            inst.inst_uid,
                            DeltaUpdateType::c_str(update_type));
}

void RefdataDeltaPublisher::publish_heartbeat() {
    ++seq_;

    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + RefDataDelta::sbeBlockLength();
    // Zero-init: heartbeat only sets a handful of fields (updateType, seq,
    // ts, instrumentId). Without zeroing, the SBE-encoded buffer leaks stack
    // garbage into the unset fields — and the instrumentType field is an
    // SBE enum that throws on any byte outside [0..3, 255]. Subscribers
    // calling .instrumentType() on the heartbeat hit the throw and Aeron's
    // error handler logs every 5 s. Caught by pricer once Deribit refdata
    // started publishing heartbeats at non-zero cadence.
    char buf[kBufSize] = {};

    uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

    RefDataDelta msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .deltaSeqNum(seq_)
        .timestampNs(now_ns)
        .updateType(DeltaUpdateType::NULL_VALUE)  // sentinel: heartbeat, not a real update
        .instrumentId(0);

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), kBufSize);
    aeron_offer(*publication_, ab, static_cast<aeron::util::index_t>(kBufSize), "heartbeat");

    bpt::common::log::debug("Published heartbeat seq={}", seq_);
}

}  // namespace bpt::refdata::messaging
