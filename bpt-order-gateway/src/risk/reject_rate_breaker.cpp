#include "order_gateway/risk/reject_rate_breaker.h"

namespace bpt::order_gateway::risk {

RejectRateBreaker::RejectRateBreaker(Config cfg) : cfg_(cfg) {}

void RejectRateBreaker::record(bool is_reject, uint64_t now_ns) {
    if (!cfg_.enabled)
        return;

    // Evict everything older than now − window_ns. The window is one-
    // sided (we accept events "at" the boundary); underflow on a
    // wall-clock regression (NTP step backwards) is guarded by the
    // signed comparison.
    const uint64_t cutoff = (now_ns > cfg_.window_ns) ? now_ns - cfg_.window_ns : 0;
    while (!events_.empty() && events_.front().first < cutoff) {
        const bool old_reject = events_.front().second;
        events_.pop_front();
        --total_;
        if (old_reject)
            --rejects_;
    }

    events_.emplace_back(now_ns, is_reject);
    ++total_;
    if (is_reject)
        ++rejects_;

    if (tripped_)
        return;  // latch — already tripped, stop checking

    if (total_ < cfg_.min_events)
        return;
    const double rate_pct = 100.0 * static_cast<double>(rejects_) / static_cast<double>(total_);
    if (rate_pct > cfg_.threshold_pct)
        tripped_ = true;
}

}  // namespace bpt::order_gateway::risk
