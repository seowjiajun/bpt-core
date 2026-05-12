#include "analytics/analysis/markout_tracker.h"

namespace bpt::analytics::analysis {

MarkoutTracker::MarkoutTracker(Config cfg) : cfg_(cfg) {}

double MarkoutTracker::compute_markout_bps(const PendingFill& pf, double mid) const {
    if (pf.fill_mid <= 0.0)
        return 0.0;
    return pf.side_sign * (mid - pf.fill_mid) / pf.fill_mid * 1e4;
}

void MarkoutTracker::on_fill(uint64_t instrument_id,
                             int side_sign,
                             double fill_price,
                             double current_mid,
                             uint64_t fill_ts_ns) {
    if (pending_.size() >= cfg_.max_pending)
        pending_.pop_front();  // evict oldest to prevent unbounded growth

    pending_.push_back({
        .instrument_id = instrument_id,
        .fill_ts_ns = fill_ts_ns,
        .side_sign = side_sign,
        .fill_price = fill_price,
        .fill_mid = current_mid,
    });
}

int MarkoutTracker::on_tick(double mid, uint64_t now_ns) {
    int completed = 0;

    auto it = pending_.begin();
    while (it != pending_.end()) {
        const uint64_t elapsed = (now_ns > it->fill_ts_ns) ? (now_ns - it->fill_ts_ns) : 0;

        if (!it->logged_1s && elapsed >= cfg_.horizon_1_ns) {
            it->markout_1s_bps = compute_markout_bps(*it, mid);
            it->logged_1s = true;
        }
        if (!it->logged_5s && elapsed >= cfg_.horizon_2_ns) {
            it->markout_5s_bps = compute_markout_bps(*it, mid);
            it->logged_5s = true;
        }
        if (!it->logged_30s && elapsed >= cfg_.horizon_3_ns) {
            it->markout_30s_bps = compute_markout_bps(*it, mid);
            it->logged_30s = true;
        }

        if (it->logged_1s && it->logged_5s && it->logged_30s) {
            completed_.push_back({
                .instrument_id = it->instrument_id,
                .fill_ts_ns = it->fill_ts_ns,
                .side_sign = it->side_sign,
                .fill_price = it->fill_price,
                .fill_mid = it->fill_mid,
                .markout_1s_bps = it->markout_1s_bps,
                .markout_5s_bps = it->markout_5s_bps,
                .markout_30s_bps = it->markout_30s_bps,
            });
            it = pending_.erase(it);
            ++completed;
        } else {
            ++it;
        }
    }

    return completed;
}

std::deque<MarkoutTracker::Observation> MarkoutTracker::consume() {
    auto result = std::move(completed_);
    completed_.clear();
    return result;
}

}  // namespace bpt::analytics::analysis
