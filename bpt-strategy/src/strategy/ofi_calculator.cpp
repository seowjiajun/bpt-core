#include "strategy/strategy/ofi_calculator.h"

#include <algorithm>
#include <cmath>

namespace bpt::strategy::strategy {

namespace {

// Per-level OFI event contribution per Cont-Kukanov-Stoikov (2014).
// Positive = buy pressure on the bid side, negative on the ask side.
//
// Bid: e_b = q_new * 1{P_new >= P_prev} - q_prev * 1{P_new <= P_prev}
//   price up  → e_b = +q_new            (fresh level above, pure add)
//   price eq  → e_b =  q_new - q_prev   (size change at same level)
//   price dn  → e_b = -q_prev           (old level evaporated)
//
// Ask is the mirror image (ask price DOWN = buy pressure):
//   price down → e_a = +q_new
//   price eq   → e_a =  q_new - q_prev
//   price up   → e_a = -q_prev
//
// Net OFI at the level = e_b - e_a (buy pressure positive).
double level_event(double new_price, double new_qty, double prev_price, double prev_qty, bool is_bid) {
    if (prev_price <= 0.0 && new_price <= 0.0)
        return 0.0;
    if (prev_price <= 0.0)
        return new_qty;  // fresh level, treat as pure add
    if (new_price <= 0.0)
        return -prev_qty;  // level removed entirely

    const bool improved = is_bid ? (new_price > prev_price) : (new_price < prev_price);
    const bool worsened = is_bid ? (new_price < prev_price) : (new_price > prev_price);

    if (improved)
        return new_qty;
    if (worsened)
        return -prev_qty;
    return new_qty - prev_qty;
}

}  // namespace

OFICalculator::OFICalculator(Config cfg) : cfg_(cfg) {}

void OFICalculator::reset() {
    prev_bids_.clear();
    prev_asks_.clear();
    window_.clear();
    running_contrib_ = 0.0;
    running_depth_ = 0.0;
    last_value_ = 0.0;
    warm_ = false;
}

double OFICalculator::update(const std::vector<Level>& bids, const std::vector<Level>& asks, uint64_t timestamp_ns) {
    // First snapshot — just store it, no signal yet.
    if (!warm_) {
        prev_bids_ = bids;
        prev_asks_ = asks;
        warm_ = true;
        return 0.0;
    }

    const int K = std::min(cfg_.max_levels, static_cast<int>(std::max(bids.size(), asks.size())));

    double contribution = 0.0;
    double depth = 0.0;

    for (int k = 0; k < K; ++k) {
        const double weight = 1.0 / static_cast<double>(k + 1);

        const bool have_new_bid = k < static_cast<int>(bids.size());
        const bool have_prev_bid = k < static_cast<int>(prev_bids_.size());
        const double new_bp = have_new_bid ? bids[k].first : 0.0;
        const double new_bq = have_new_bid ? bids[k].second : 0.0;
        const double prev_bp = have_prev_bid ? prev_bids_[k].first : 0.0;
        const double prev_bq = have_prev_bid ? prev_bids_[k].second : 0.0;
        const double e_b = level_event(new_bp, new_bq, prev_bp, prev_bq, /*is_bid*/ true);

        const bool have_new_ask = k < static_cast<int>(asks.size());
        const bool have_prev_ask = k < static_cast<int>(prev_asks_.size());
        const double new_ap = have_new_ask ? asks[k].first : 0.0;
        const double new_aq = have_new_ask ? asks[k].second : 0.0;
        const double prev_ap = have_prev_ask ? prev_asks_[k].first : 0.0;
        const double prev_aq = have_prev_ask ? prev_asks_[k].second : 0.0;
        const double e_a = level_event(new_ap, new_aq, prev_ap, prev_aq, /*is_bid*/ false);

        contribution += weight * (e_b - e_a);
        depth += new_bq + new_aq;
    }

    // Evict samples outside the rolling window before adding the new one.
    const uint64_t cutoff = (timestamp_ns > cfg_.window_ns) ? (timestamp_ns - cfg_.window_ns) : 0;
    while (!window_.empty() && window_.front().ts_ns < cutoff) {
        running_contrib_ -= window_.front().contribution;
        running_depth_ -= window_.front().depth;
        window_.pop_front();
    }

    window_.push_back({timestamp_ns, contribution, depth});
    running_contrib_ += contribution;
    running_depth_ += depth;

    prev_bids_ = bids;
    prev_asks_ = asks;

    // Normalise: raw running_contrib_ is in units of (queue size per
    // weighted level). Dividing by the average top-K depth over the
    // same window makes the value unit-free and comparable across
    // instruments. Guard against division by zero on near-empty books.
    const double avg = avg_depth();
    last_value_ = (avg > 1e-9) ? (running_contrib_ / avg) : 0.0;
    return last_value_;
}

double OFICalculator::avg_depth() const {
    if (window_.empty())
        return 0.0;
    return running_depth_ / static_cast<double>(window_.size());
}

}  // namespace bpt::strategy::strategy
