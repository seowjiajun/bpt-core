#include "pricer/messaging/vol_surface_publisher.h"

#include <messages/MessageHeader.h>
#include <messages/VolSurface.h>

#include <cstring>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>

namespace bpt::pricer::messaging {

VolSurfacePublisher::VolSurfacePublisher(std::shared_ptr<aeron::Aeron> aeron,
                                         const std::string& channel,
                                         int32_t stream_id) {
    pub_ = ygg::aeron::wait_for_publication(aeron, channel, stream_id);
    ygg::log::info("[VolSurfacePublisher] Publication ready on {} stream {}", channel, stream_id);
}

void VolSurfacePublisher::publish(const surface::VolSurfaceGrid& grid, uint64_t timestamp_ns) {
    if (!pub_)
        return;

    // Each point is ~140 bytes with all fields. 400 points ≈ 56KB.
    constexpr size_t kMaxBuf = 65536;
    alignas(8) char buf[kMaxBuf];
    std::memset(buf, 0, kMaxBuf);

    using namespace bpt::messages;

    MessageHeader hdr;
    VolSurface msg;

    hdr.wrap(buf, 0, VolSurface::sbeSchemaVersion(), kMaxBuf)
        .blockLength(VolSurface::sbeBlockLength())
        .templateId(VolSurface::sbeTemplateId())
        .schemaId(VolSurface::sbeSchemaId())
        .version(VolSurface::sbeSchemaVersion());

    msg.wrapForEncode(buf, hdr.encodedLength(), kMaxBuf);
    msg.timestampNs(timestamp_ns);
    msg.exchangeId(grid.exchange_id);

    msg.putUnderlying(grid.underlying);

    msg.seqNum(grid.seq_num);

    auto& pts = msg.pointsCount(static_cast<uint16_t>(grid.points.size()));
    for (const auto& p : grid.points) {
        pts.next()
            .instrumentId(p.instrument_id)
            .expiryDate(p.expiry_date)
            .strikePrice(p.strike_price)
            .optionSide(p.option_side)
            .impliedVol(p.implied_vol)
            .forwardPrice(p.forward_price)
            .timeToExpiry(p.time_to_expiry)
            .bidIv(p.bid_iv)
            .askIv(p.ask_iv)
            .bidPrice(p.bid_price)
            .askPrice(p.ask_price)
            .delta(p.delta)
            .gamma(p.gamma)
            .vega(p.vega)
            .theta(p.theta);
    }

    const auto total = MessageHeader::encodedLength() + msg.encodedLength();
    aeron::concurrent::AtomicBuffer buffer(reinterpret_cast<uint8_t*>(buf), total);
    pub_->offer(buffer, 0, static_cast<int32_t>(total));
}

}  // namespace bpt::pricer::messaging
