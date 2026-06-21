#include "md_gateway/messaging/publishers/md_publisher.h"

#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::md_gateway::messaging {

namespace sbe = bpt::messages;

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("MdPublisher");
    return l;
}
}  // namespace

using Policy = bpt::common::aeron::Publisher::Policy;

MdPublisher::MdPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                         const bpt::common::config::StreamConfig& stream,
                         double max_price_deviation_pct,
                         md::ValidationDropBreaker::Config breaker_cfg,
                         std::string adapter_name)
    // Latency-critical MD fan-out. On back-pressure, drop rather than
    // delay — a slow consumer shouldn't wedge the market-data path.
    : publisher_(std::move(aeron), stream.channel, stream.stream_id, Policy::kDropAlways),
      validator_(max_price_deviation_pct),
      breaker_(breaker_cfg),
      adapter_name_(std::move(adapter_name)) {}

bool MdPublisher::check_breaker_or_drop() {
    if (breaker_.tripped()) {
        validation_drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void MdPublisher::record_validation_and_breaker(bool is_drop) {
    const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
    const bool was_tripped = breaker_.tripped();
    breaker_.record(is_drop, now_ns);
    if (!was_tripped && breaker_.tripped()) {
        bpt::common::log::error(
            "{} VALIDATION-DROP BREAKER TRIPPED — {}/{} publishes "
            "dropped in last {}s (threshold {:.1f}%). Publishing suppressed. "
            "Restart service after human review to resume.",
            adapter_name_,
            breaker_.drops_in_window(),
            breaker_.total_in_window(),
            breaker_.config().window_ns / 1'000'000'000ULL,
            breaker_.config().threshold_pct);
    }
    if (is_drop)
        validation_drops_.fetch_add(1, std::memory_order_relaxed);
}

void MdPublisher::publish(const md::MdBbo& bbo) {
    if (!check_breaker_or_drop())
        return;
    const bool is_drop = (validator_.validate(bbo) != md::ValidationResult::OK);
    record_validation_and_breaker(is_drop);
    if (is_drop)
        return;

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
    if (ok)
        published_.fetch_add(1, std::memory_order_relaxed);
    else
        record_backpressure_drop(bbo.instrument_id, "BBO");
}

void MdPublisher::publish(const md::MdTrade& trade) {
    if (!check_breaker_or_drop())
        return;
    const bool is_drop = (validator_.validate(trade) != md::ValidationResult::OK);
    record_validation_and_breaker(is_drop);
    if (is_drop)
        return;

    const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool ok = publisher_.publish<sbe::MdTrade>([&](sbe::MdTrade& msg) {
        msg.timestampNs(trade.timestamp_ns)
            .instrumentId(trade.instrument_id)
            .price(trade.price)
            .qty(trade.qty)
            .side(trade.side)
            .seqNum(seq);
    });
    if (ok)
        published_.fetch_add(1, std::memory_order_relaxed);
    else
        record_backpressure_drop(trade.instrument_id, "Trade");
}

void MdPublisher::publish(const md::MdOrderBook& book) {
    if (!check_breaker_or_drop())
        return;
    const bool is_drop = (validator_.validate(book) != md::ValidationResult::OK);
    record_validation_and_breaker(is_drop);
    if (is_drop)
        return;

    // OrderBook stays on offer(): variable-size payload (up to ~2KB
    // depending on level count), so length isn't compile-time known and
    // tryClaim's fixed-length contract doesn't fit.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    char buf[md::MdEncoder::kMaxOrderBookBufSize];
    std::size_t len = md::MdEncoder::encode(book, seq_.fetch_add(1, std::memory_order_relaxed) + 1, buf, sizeof(buf));
    if (len == 0)
        return;
    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<::aeron::util::index_t>(len));
    if (publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(len)))
        published_.fetch_add(1, std::memory_order_relaxed);
    else
        record_backpressure_drop(book.instrument_id, "OrderBook");
}

void MdPublisher::record_backpressure_drop(uint64_t instrument_id, const char* label) {
    uint64_t d = backpressure_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (d <= 5 || d % 1000 == 0)
        bpt::common::log::warn(kLog(), "{} dropped (backpressure): id={} drops={}", label, instrument_id, d);
}

}  // namespace bpt::md_gateway::messaging
