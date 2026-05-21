#include "features/order_book_state.h"

namespace bpt::features {

namespace {
template <class Map>
void fold_into(Map& side, const std::vector<OrderBookState::Level>& deltas) {
    for (const auto& lvl : deltas) {
        if (lvl.qty > 0.0) {
            side[lvl.price] = lvl.qty;
        } else {
            side.erase(lvl.price);
        }
    }
}
}  // namespace

void OrderBookState::apply(const bpt::messages::MdOrderBook& book, bool is_snapshot) {
    // SBE repeating groups hold an internal cursor, so the reference must
    // be non-const. The SBE codec does not mutate the payload — this is a
    // decode-side position pointer, identical to the pattern in
    // ofi_strategy.cpp and hmm_strategy.cpp.
    auto& mutable_book = const_cast<bpt::messages::MdOrderBook&>(book);

    std::vector<Level> bids;
    auto& bids_grp = mutable_book.bids();
    bids.reserve(bids_grp.count());
    while (bids_grp.hasNext()) {
        auto& lvl = bids_grp.next();
        bids.push_back({lvl.price(), lvl.qty()});
    }

    std::vector<Level> asks;
    auto& asks_grp = mutable_book.asks();
    asks.reserve(asks_grp.count());
    while (asks_grp.hasNext()) {
        auto& lvl = asks_grp.next();
        asks.push_back({lvl.price(), lvl.qty()});
    }

    apply(bids, asks, book.seqNum(), book.timestampNs(), is_snapshot);
}

void OrderBookState::apply(const std::vector<Level>& bid_levels,
                           const std::vector<Level>& ask_levels,
                           uint64_t seq_num,
                           uint64_t timestamp_ns,
                           bool is_snapshot) {
    if (is_snapshot) {
        bids_.clear();
        asks_.clear();
    }
    fold_into(bids_, bid_levels);
    fold_into(asks_, ask_levels);
    last_seq_num_ = seq_num;
    last_update_ns_ = timestamp_ns;
}

void OrderBookState::reset() {
    bids_.clear();
    asks_.clear();
    last_seq_num_ = 0;
    last_update_ns_ = 0;
}

double OrderBookState::best_bid() const {
    return bids_.begin()->first;
}
double OrderBookState::best_ask() const {
    return asks_.begin()->first;
}
double OrderBookState::best_bid_qty() const {
    return bids_.begin()->second;
}
double OrderBookState::best_ask_qty() const {
    return asks_.begin()->second;
}
double OrderBookState::mid() const {
    return 0.5 * (best_bid() + best_ask());
}

double OrderBookState::size_at_bid(double price) const {
    auto it = bids_.find(price);
    return it == bids_.end() ? 0.0 : it->second;
}

double OrderBookState::size_at_ask(double price) const {
    auto it = asks_.find(price);
    return it == asks_.end() ? 0.0 : it->second;
}

double OrderBookState::bid_vol_above(double price) const {
    // bids_ uses std::greater<>, so lower_bound(price) returns the first
    // element with key <= price (in the descending-sort sense). Everything
    // before it has key strictly > price.
    double total = 0.0;
    for (auto it = bids_.begin(); it != bids_.lower_bound(price); ++it)
        total += it->second;
    return total;
}

double OrderBookState::ask_vol_below(double price) const {
    // asks_ uses default less<>. lower_bound(price) returns the first
    // element with key >= price. Everything before is strictly < price.
    double total = 0.0;
    for (auto it = asks_.begin(); it != asks_.lower_bound(price); ++it)
        total += it->second;
    return total;
}

void OrderBookState::top_bids(size_t n, std::vector<Level>& out) const {
    out.clear();
    size_t i = 0;
    for (auto& [px, qty] : bids_) {
        if (i++ >= n)
            break;
        out.push_back({px, qty});
    }
}

void OrderBookState::top_asks(size_t n, std::vector<Level>& out) const {
    out.clear();
    size_t i = 0;
    for (auto& [px, qty] : asks_) {
        if (i++ >= n)
            break;
        out.push_back({px, qty});
    }
}

std::vector<OrderBookState::Level> OrderBookState::top_bids(size_t n) const {
    std::vector<Level> out;
    out.reserve(std::min(n, bids_.size()));
    top_bids(n, out);
    return out;
}

std::vector<OrderBookState::Level> OrderBookState::top_asks(size_t n) const {
    std::vector<Level> out;
    out.reserve(std::min(n, asks_.size()));
    top_asks(n, out);
    return out;
}

}  // namespace bpt::features
