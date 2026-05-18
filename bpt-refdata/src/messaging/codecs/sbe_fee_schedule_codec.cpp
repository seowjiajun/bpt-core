#include "refdata/messaging/codecs/sbe_fee_schedule_codec.h"

#include <messages/FeeSchedule.h>
#include <messages/MessageHeader.h>

#include <cstring>
#include <stdexcept>

namespace bpt::refdata::messaging {

using bpt::messages::FeeSchedule;
using bpt::messages::MessageHeader;

std::span<const std::byte> SbeFeeScheduleCodec::encode(const model::FeeScheduleState& fs,
                                                       std::span<std::byte> scratch) {
    auto* buf = reinterpret_cast<char*>(scratch.data());
    std::memset(buf, 0, scratch.size());

    FeeSchedule msg;
    msg.wrapAndApplyHeader(buf, 0, scratch.size())
        .exchangeId(fs.exchange_id)
        .instrumentId(fs.instrument_id)
        .instrumentType(fs.instrument_type)
        .makerFeeBps(fs.maker_fee_bps)
        .takerFeeBps(fs.taker_fee_bps)
        .updatedTs(fs.updated_ts);

    const auto total = MessageHeader::encodedLength() + FeeSchedule::sbeBlockLength();
    return scratch.subspan(0, total);
}

model::FeeScheduleState SbeFeeScheduleCodec::decode(std::span<const std::byte> bytes) {
    if (bytes.size() < MessageHeader::encodedLength())
        throw std::runtime_error("SbeFeeScheduleCodec::decode: too short");

    auto* data = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
    const uint64_t len = bytes.size();

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), len);

    if (hdr.templateId() != FeeSchedule::sbeTemplateId())
        throw std::runtime_error("SbeFeeScheduleCodec::decode: wrong template id");

    FeeSchedule msg;
    msg.wrapForDecode(data, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), len);

    model::FeeScheduleState out;
    out.exchange_id = msg.exchangeId();
    out.instrument_id = msg.instrumentId();
    out.instrument_type = msg.instrumentType();
    out.maker_fee_bps = msg.makerFeeBps();
    out.taker_fee_bps = msg.takerFeeBps();
    out.updated_ts = msg.updatedTs();
    return out;
}

}  // namespace bpt::refdata::messaging
