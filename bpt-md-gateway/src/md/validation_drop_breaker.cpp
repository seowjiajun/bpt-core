#include "md_gateway/md/validation_drop_breaker.h"

namespace bpt::md_gateway::md {

ValidationDropBreaker::ValidationDropBreaker(Config cfg) : cfg_(cfg) {}

void ValidationDropBreaker::record(bool is_drop, uint64_t now_ns) {
    if (!cfg_.enabled)
        return;

    // Evict events older than now - window_ns. Guard against NTP step
    // backward — same pattern as RejectRateBreaker in order-gateway.
    const uint64_t cutoff = (now_ns > cfg_.window_ns) ? now_ns - cfg_.window_ns : 0;
    while (!events_.empty() && events_.front().first < cutoff) {
        const bool old_drop = events_.front().second;
        events_.pop_front();
        --total_;
        if (old_drop)
            --drops_;
    }

    events_.emplace_back(now_ns, is_drop);
    ++total_;
    if (is_drop)
        ++drops_;

    if (tripped_)
        return;  // latched
    if (total_ < cfg_.min_events)
        return;
    const double rate_pct = 100.0 * static_cast<double>(drops_) / static_cast<double>(total_);
    if (rate_pct > cfg_.threshold_pct)
        tripped_ = true;
}

}  // namespace bpt::md_gateway::md
