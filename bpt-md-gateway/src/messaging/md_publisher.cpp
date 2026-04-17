#include "md_gateway/messaging/md_publisher.h"

#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>

namespace bpt::md_gateway::messaging {

MdPublisher::MdPublisher(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id) {
    publication_ = ygg::aeron::wait_for_publication(aeron, channel, stream_id);
}

void MdPublisher::publish(const md::MdBbo& bbo) {
    // No zero-init: MdEncoder writes every byte up to `len`; offer() reads only `len` bytes.
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

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(const_cast<char*>(buf)), static_cast<aeron::util::index_t>(len));

    if (publication_->offer(ab, 0, static_cast<aeron::util::index_t>(len)) < 0) {
        uint64_t d = drops_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (d <= 5 || d % 1000 == 0)
            ygg::log::warn("[MdPublisher] {} dropped (backpressure): id={} drops={}", label, instrument_id, d);
    }
}

}  // namespace bpt::md_gateway::messaging
