#include "md_gateway/md/md_validator.h"

#include <bpt_common/logging.h>
#include <cmath>

namespace bpt::md_gateway::md {

namespace {
// Sub-module logger — auto-prefixed with "MdValidator" via %(logger).
// Lazy-init because bpt::common::logging::init() runs after static init.
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("MdValidator");
    return l;
}
}  // namespace

MdValidator::MdValidator(double max_price_deviation_pct) : max_deviation_ratio_(max_price_deviation_pct / 100.0) {}

void MdValidator::reset() {
    last_mid_.clear();
    drop_count_.clear();
}

// Log at most once per 100 drops per instrument to avoid flooding on a
// persistently misbehaving feed.
static bool should_log(std::unordered_map<uint64_t, uint32_t>& counts, uint64_t id) {
    uint32_t n = ++counts[id];
    return n == 1 || n % 100 == 0;
}

ValidationResult MdValidator::validate(const MdBbo& bbo) {
    if (bbo.bid_price <= 0.0 || bbo.ask_price <= 0.0) {
        if (should_log(drop_count_, bbo.instrument_id))
            bpt::common::log::warn(kLog(),
                                   "BBO invalid prices: bid={} ask={} id={}",
                                   bbo.bid_price,
                                   bbo.ask_price,
                                   bbo.instrument_id);
        return ValidationResult::DROP;
    }
    if (bbo.bid_qty <= 0.0 || bbo.ask_qty <= 0.0) {
        if (should_log(drop_count_, bbo.instrument_id))
            bpt::common::log::warn(kLog(),
                                   "BBO invalid quantities: bid_qty={} ask_qty={} id={}",
                                   bbo.bid_qty,
                                   bbo.ask_qty,
                                   bbo.instrument_id);
        return ValidationResult::DROP;
    }
    if (bbo.ask_price <= bbo.bid_price) {
        if (should_log(drop_count_, bbo.instrument_id))
            bpt::common::log::warn(kLog(),
                                   "BBO crossed: bid={} ask={} id={}",
                                   bbo.bid_price,
                                   bbo.ask_price,
                                   bbo.instrument_id);
        return ValidationResult::DROP;
    }

    double mid = (bbo.bid_price + bbo.ask_price) * 0.5;

    auto it = last_mid_.find(bbo.instrument_id);
    if (it != last_mid_.end()) {
        double deviation = std::abs(mid - it->second) / it->second;
        if (deviation > max_deviation_ratio_) {
            if (should_log(drop_count_, bbo.instrument_id))
                bpt::common::log::warn(kLog(),
                                       "BBO price deviation {:.2f}% > {:.2f}%: mid={} last={} id={}",
                                       deviation * 100.0,
                                       max_deviation_ratio_ * 100.0,
                                       mid,
                                       it->second,
                                       bbo.instrument_id);
            return ValidationResult::DROP;
        }
        it->second = mid;
    } else {
        last_mid_.emplace(bbo.instrument_id, mid);
    }

    return ValidationResult::OK;
}

ValidationResult MdValidator::validate(const MdTrade& trade) {
    if (trade.price <= 0.0) {
        if (should_log(drop_count_, trade.instrument_id))
            bpt::common::log::warn(kLog(), "Trade invalid price={} id={}", trade.price, trade.instrument_id);
        return ValidationResult::DROP;
    }
    if (trade.qty <= 0.0) {
        if (should_log(drop_count_, trade.instrument_id))
            bpt::common::log::warn(kLog(), "Trade invalid qty={} id={}", trade.qty, trade.instrument_id);
        return ValidationResult::DROP;
    }

    auto it = last_mid_.find(trade.instrument_id);
    if (it != last_mid_.end()) {
        double deviation = std::abs(trade.price - it->second) / it->second;
        if (deviation > max_deviation_ratio_) {
            if (should_log(drop_count_, trade.instrument_id))
                bpt::common::log::warn(kLog(),
                                       "Trade price deviation {:.2f}% > {:.2f}%: px={} last_mid={} "
                                       "id={}",
                                       deviation * 100.0,
                                       max_deviation_ratio_ * 100.0,
                                       trade.price,
                                       it->second,
                                       trade.instrument_id);
            return ValidationResult::DROP;
        }
    }

    return ValidationResult::OK;
}

ValidationResult MdValidator::validate(const MdOrderBook& book) {
    if (book.bids.empty() || book.asks.empty()) {
        if (should_log(drop_count_, book.instrument_id))
            bpt::common::log::warn(kLog(), "OrderBook empty side id={}", book.instrument_id);
        return ValidationResult::DROP;
    }

    double best_bid = book.bids[0].first;
    double best_ask = book.asks[0].first;

    if (best_bid <= 0.0 || best_ask <= 0.0) {
        if (should_log(drop_count_, book.instrument_id))
            bpt::common::log::warn(kLog(),
                                   "OrderBook invalid prices: bid={} ask={} id={}",
                                   best_bid,
                                   best_ask,
                                   book.instrument_id);
        return ValidationResult::DROP;
    }
    if (best_ask <= best_bid) {
        if (should_log(drop_count_, book.instrument_id))
            bpt::common::log::warn(kLog(),
                                   "OrderBook crossed: bid={} ask={} id={}",
                                   best_bid,
                                   best_ask,
                                   book.instrument_id);
        return ValidationResult::DROP;
    }

    for (std::size_t i = 1; i < book.bids.size(); ++i) {
        if (book.bids[i].first >= book.bids[i - 1].first) {
            if (should_log(drop_count_, book.instrument_id))
                bpt::common::log::warn(kLog(), "OrderBook bids not descending id={}", book.instrument_id);
            return ValidationResult::DROP;
        }
    }
    for (std::size_t i = 1; i < book.asks.size(); ++i) {
        if (book.asks[i].first <= book.asks[i - 1].first) {
            if (should_log(drop_count_, book.instrument_id))
                bpt::common::log::warn(kLog(), "OrderBook asks not ascending id={}", book.instrument_id);
            return ValidationResult::DROP;
        }
    }

    // Check deviation against last known mid (updated on BBO publish, not here).
    auto it = last_mid_.find(book.instrument_id);
    if (it != last_mid_.end()) {
        double mid = (best_bid + best_ask) * 0.5;
        double deviation = std::abs(mid - it->second) / it->second;
        if (deviation > max_deviation_ratio_) {
            if (should_log(drop_count_, book.instrument_id))
                bpt::common::log::warn(kLog(),
                                       "OrderBook price deviation {:.2f}% > {:.2f}%: mid={} last={} "
                                       "id={}",
                                       deviation * 100.0,
                                       max_deviation_ratio_ * 100.0,
                                       mid,
                                       it->second,
                                       book.instrument_id);
            return ValidationResult::DROP;
        }
    }

    return ValidationResult::OK;
}

}  // namespace bpt::md_gateway::md
