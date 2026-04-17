#include "md_gateway/adapter/common/subscription_map.h"

#include <mutex>

namespace bpt::md_gateway::adapter {

void SubscriptionMap::subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth) {
    std::unique_lock lock(mu_);
    bool is_new = by_id_.find(instrument_id) == by_id_.end();
    by_id_[instrument_id] = Entry{symbol, depth};
    by_symbol_[symbol] = instrument_id;
    if (is_new)
        pending_.push_back(Entry{std::move(symbol), depth});
}

std::string SubscriptionMap::unsubscribe(uint64_t instrument_id) {
    std::unique_lock lock(mu_);
    auto it = by_id_.find(instrument_id);
    if (it == by_id_.end())
        return {};
    std::string symbol = std::move(it->second.symbol);
    by_id_.erase(it);
    by_symbol_.erase(symbol);
    return symbol;
}

void SubscriptionMap::requeue(const std::string& symbol) {
    std::unique_lock lock(mu_);
    auto sit = by_symbol_.find(symbol);
    if (sit == by_symbol_.end())
        return;
    auto eit = by_id_.find(sit->second);
    if (eit != by_id_.end())
        pending_.push_back(eit->second);
}

uint64_t SubscriptionMap::find_id(std::string_view symbol) const {
    std::shared_lock lock(mu_);
    auto it = by_symbol_.find(symbol);
    return (it != by_symbol_.end()) ? it->second : 0;
}

uint8_t SubscriptionMap::find_depth(uint64_t instrument_id) const {
    std::shared_lock lock(mu_);
    auto it = by_id_.find(instrument_id);
    return (it != by_id_.end()) ? it->second.depth : 0;
}

SubscriptionMap::FindResult SubscriptionMap::find(std::string_view symbol) const {
    std::shared_lock lock(mu_);
    auto sit = by_symbol_.find(symbol);
    if (sit == by_symbol_.end())
        return {};
    auto eit = by_id_.find(sit->second);
    if (eit == by_id_.end())
        return {};
    return {eit->first, eit->second.depth};
}

std::vector<std::pair<uint64_t, SubscriptionMap::Entry>> SubscriptionMap::snapshot() const {
    std::shared_lock lock(mu_);
    return {by_id_.begin(), by_id_.end()};
}

std::vector<SubscriptionMap::Entry> SubscriptionMap::take_pending() {
    std::unique_lock lock(mu_);
    std::vector<Entry> out;
    out.swap(pending_);
    return out;
}

}  // namespace bpt::md_gateway::adapter
