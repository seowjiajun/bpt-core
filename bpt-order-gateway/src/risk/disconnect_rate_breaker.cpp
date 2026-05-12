#include "order_gateway/risk/disconnect_rate_breaker.h"

namespace bpt::order_gateway::risk {

DisconnectRateBreaker::DisconnectRateBreaker(Config cfg) : cfg_(cfg) {}

void DisconnectRateBreaker::reset(Config cfg) {
    cfg_ = cfg;
    events_.clear();
    tripped_.store(false, std::memory_order_release);
}

void DisconnectRateBreaker::record(uint64_t now_ns) {
    if (!cfg_.enabled)
        return;

    // Evict older-than-window events. Underflow guard mirrors
    // RejectRateBreaker's — a backward NTP step can't produce a
    // nonsensical cutoff.
    const uint64_t cutoff = (now_ns > cfg_.window_ns) ? now_ns - cfg_.window_ns : 0;
    while (!events_.empty() && events_.front() < cutoff)
        events_.pop_front();

    events_.push_back(now_ns);

    if (tripped_.load(std::memory_order_relaxed))
        return;  // already latched
    if (events_.size() >= cfg_.threshold)
        tripped_.store(true, std::memory_order_release);
}

}  // namespace bpt::order_gateway::risk
