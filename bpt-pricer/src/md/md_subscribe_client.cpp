#include "pricer/md/md_subscribe_client.h"

#include <messages/MdSubscribeBatch.h>
#include <messages/MessageHeader.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <cstring>
#include <vector>

namespace bpt::pricer::md {

using bpt::messages::MdSubscribeBatch;
using bpt::messages::MessageHeader;
using Policy = bpt::common::aeron::Publisher::Policy;

MdSubscribeClient::MdSubscribeClient(std::shared_ptr<::aeron::Aeron> aeron,
                                     const std::string& channel,
                                     int stream_id)
    : publisher_(std::make_unique<bpt::common::aeron::Publisher>(std::move(aeron),
                                                                 channel,
                                                                 stream_id,
                                                                 Policy::kRetryOnBackpressure)) {
    bpt::common::log::info("[MdSubscribeClient] control publication ready on stream {}", stream_id);
}

void MdSubscribeClient::publish(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) {
    const auto n = static_cast<uint16_t>(instruments.size());

    const std::size_t buf_size = MessageHeader::encodedLength() + MdSubscribeBatch::sbeBlockLength() +
                                 MdSubscribeBatch::Instruments::sbeHeaderSize() +
                                 static_cast<std::size_t>(n) * MdSubscribeBatch::Instruments::sbeBlockLength();

    std::vector<char> buf(buf_size, '\0');

    MdSubscribeBatch msg;
    msg.wrapAndApplyHeader(buf.data(), 0, buf_size)
        .correlationId(correlation_id)
        .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

    auto& g = msg.instrumentsCount(n);
    for (const auto& inst : instruments) {
        g.next()
            .instrumentId(inst.instrument_id)
            .putExchange(inst.exchange.c_str())
            .putSymbol(inst.symbol.c_str())
            .depth(inst.depth);
    }

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf.data()), static_cast<::aeron::util::index_t>(buf_size));
    publisher_->offer(ab, 0, static_cast<::aeron::util::index_t>(buf_size));

    bpt::common::log::info("[MdSubscribeClient] sent batch correlation_id={} instruments={}", correlation_id, n);
}

}  // namespace bpt::pricer::md
