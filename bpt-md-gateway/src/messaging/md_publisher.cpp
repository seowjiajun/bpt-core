#include "md_gateway/messaging/md_publisher.h"

#include <bpt_common/logging.h>

namespace bpt::md_gateway::messaging {

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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[md::MdEncoder::kBboBufSize];
    std::size_t len = md::MdEncoder::encode(bbo, seq_.fetch_add(1, std::memory_order_relaxed) + 1, buf, sizeof(buf));
    offer(buf, len, bbo.instrument_id, "BBO");
}

void MdPublisher::publish(const md::MdTrade& trade) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[md::MdEncoder::kTradeBufSize];
    std::size_t len = md::MdEncoder::encode(trade, seq_.fetch_add(1, std::memory_order_relaxed) + 1, buf, sizeof(buf));
    offer(buf, len, trade.instrument_id, "Trade");
}

void MdPublisher::publish(const md::MdOrderBook& book) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[md::MdEncoder::kMaxOrderBookBufSize];
    std::size_t len = md::MdEncoder::encode(book, seq_.fetch_add(1, std::memory_order_relaxed) + 1, buf, sizeof(buf));
    offer(buf, len, book.instrument_id, "OrderBook");
}

void MdPublisher::offer(const char* buf, std::size_t len, uint64_t instrument_id, const char* label) {
    if (len == 0)
        return;

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(const_cast<char*>(buf)),
                             static_cast<::aeron::util::index_t>(len));
    if (!publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(len))) {
        uint64_t d = drops_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (d <= 5 || d % 1000 == 0)
            bpt::common::log::warn(kLog(), "{} dropped (backpressure): id={} drops={}", label, instrument_id, d);
    }
}

}  // namespace bpt::md_gateway::messaging
