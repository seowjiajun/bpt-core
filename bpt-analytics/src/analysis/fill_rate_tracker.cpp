#include "analytics/analysis/fill_rate_tracker.h"

#include <cmath>
#include <limits>

namespace bpt::analytics::analysis {

void FillRateTracker::on_acked(uint64_t order_id, int side_sign, uint64_t ack_ns) {
    pending_[order_id] = {side_sign, ack_ns};
}

void FillRateTracker::on_filled(uint64_t order_id, uint64_t fill_ns) {
    auto it = pending_.find(order_id);
    if (it == pending_.end())
        return;

    const double ttf_ms = static_cast<double>(fill_ns - it->second.ack_ns) / 1e6;

    window_.push_back({it->second.side_sign, true, ttf_ms});
    pending_.erase(it);

    while (window_.size() > cfg_.window_size)
        window_.pop_front();
}

void FillRateTracker::on_cancelled(uint64_t order_id, uint64_t cancel_ns) {
    auto it = pending_.find(order_id);
    if (it == pending_.end())
        return;

    window_.push_back({it->second.side_sign, false, 0.0});
    pending_.erase(it);

    while (window_.size() > cfg_.window_size)
        window_.pop_front();
}

FillRateTracker::SideStats FillRateTracker::stats(int side_sign) const {
    uint32_t fills = 0;
    uint32_t cancels = 0;
    double ttf_sum = 0.0;

    for (const auto& o : window_) {
        if (o.side_sign != side_sign)
            continue;
        if (o.filled) {
            ++fills;
            ttf_sum += o.ttf_ms;
        } else {
            ++cancels;
        }
    }

    uint32_t total = fills + cancels;
    SideStats s;
    s.fills = fills;
    s.cancels = cancels;
    s.total = total;
    s.fill_rate = total > 0 ? static_cast<double>(fills) / total : std::numeric_limits<double>::quiet_NaN();
    s.mean_ttf_ms = fills > 0 ? ttf_sum / fills : std::numeric_limits<double>::quiet_NaN();
    return s;
}

}  // namespace bpt::analytics::analysis
