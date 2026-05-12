#include "md_gateway/messaging/md_publisher.h"

#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>

#include <bpt_common/logging.h>

namespace bpt::md_gateway::messaging {

namespace sbe = bpt::messages;

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("MdPublisher");
    return l;
}
}  // namespace

using Policy = bpt::common::aeron::Publisher::Policy;

MdPublisher::MdPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id)
    // Latency-critical MD fan-out. On back-pressure, drop rather than
    // delay — a slow consumer shouldn't wedge the market-data path.
    : publisher_(std::move(aeron), channel, stream_id, Policy::kDropAlways) {}

void MdPublisher::publish(const md::MdBbo& bbo) {
    const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool ok = publisher_.publish<sbe::MdMarketData>([&](sbe::MdMarketData& msg) {
        msg.timestampNs(bbo.timestamp_ns)
            .instrumentId(bbo.instrument_id)
            .bidPrice(bbo.bid_price)
            .bidQty(bbo.bid_qty)
            .askPrice(bbo.ask_price)
            .askQty(bbo.ask_qty)
            .seqNum(seq);
    });
    if (!ok)
        record_drop(bbo.instrument_id, "BBO");
}

void MdPublisher::publish(const md::MdTrade& trade) {
    const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool ok = publisher_.publish<sbe::MdTrade>([&](sbe::MdTrade& msg) {
        msg.timestampNs(trade.timestamp_ns)
            .instrumentId(trade.instrument_id)
            .price(trade.price)
            .qty(trade.qty)
            .side(trade.side)
            .seqNum(seq);
    });
    if (!ok)
        record_drop(trade.instrument_id, "Trade");
}

void MdPublisher::publish(const md::MdOrderBook& book) {
    // OrderBook stays on offer(): variable-size payload (up to ~2KB
    // depending on level count), so length isn't compile-time known and
    // tryClaim's fixed-length contract doesn't fit.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[md::MdEncoder::kMaxOrderBookBufSize];
    std::size_t len = md::MdEncoder::encode(book, seq_.fetch_add(1, std::memory_order_relaxed) + 1, buf, sizeof(buf));
    if (len == 0)
        return;
    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<::aeron::util::index_t>(len));
    if (!publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(len)))
        record_drop(book.instrument_id, "OrderBook");
}

void MdPublisher::record_drop(uint64_t instrument_id, const char* label) {
    uint64_t d = drops_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (d <= 5 || d % 1000 == 0)
        bpt::common::log::warn(kLog(), "{} dropped (backpressure): id={} drops={}", label, instrument_id, d);
}

}  // namespace bpt::md_gateway::messaging
