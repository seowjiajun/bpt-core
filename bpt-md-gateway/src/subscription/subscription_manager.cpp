#include "md_gateway/subscription/subscription_manager.h"

#include <messages/AckStatus.h>
#include <messages/MessageHeader.h>

#include <bpt_common/logging.h>
#include <cstring>
#include <unordered_set>

namespace bpt::md_gateway::subscription {

using bpt::messages::AckStatus;
using bpt::messages::MdSubscribeBatch;
using bpt::messages::MessageHeader;

void SubscriptionManager::add_adapter(std::shared_ptr<adapter::IAdapter> adapter) {
    adapters_.push_back(std::move(adapter));
}

adapter::IAdapter* SubscriptionManager::find_adapter(const std::string& exchange) {
    for (auto& a : adapters_) {
        if (exchange == a->exchange_name())
            return a.get();
    }
    return nullptr;
}

void SubscriptionManager::apply_requests(uint64_t correlation_id,
                                         const std::vector<SubscribeRequest>& desired,
                                         messaging::IAckPublisher& ack_pub) {
    // Build this consumer's new desired set (by instrument_id).
    std::unordered_set<uint64_t> new_set;
    new_set.reserve(desired.size());
    for (const auto& d : desired)
        new_set.insert(d.instrument_id);

    // Diff against this consumer's previous set: instruments it used to want
    // but no longer wants must be removed from those instruments' wanters.
    // If the wanters set becomes empty, the venue subscription drops.
    auto& prev_set = per_consumer_[correlation_id];
    for (uint64_t old_id : prev_set) {
        if (new_set.find(old_id) != new_set.end())
            continue;  // still wanted by this consumer
        auto it = active_.find(old_id);
        if (it == active_.end())
            continue;
        it->second.wanters.erase(correlation_id);
        if (it->second.wanters.empty()) {
            adapter::IAdapter* adapter = find_adapter(it->second.exchange);
            if (adapter)
                adapter->unsubscribe(old_id);
            bpt::common::log::info("SubscriptionManager: unsubscribed {} on {} (last wanter dropped)",
                                   old_id,
                                   it->second.exchange);
            active_.erase(it);
        }
    }
    prev_set = new_set;  // commit this consumer's new desired set

    // For each instrument in the new desired set: add this consumer to the
    // wanters; if the wanters set was previously empty (or absent), this is
    // a fresh venue subscription.
    for (const auto& d : desired) {
        adapter::IAdapter* adapter = find_adapter(d.exchange);
        if (!adapter) {
            bpt::common::log::warn("SubscriptionManager: no adapter for exchange {}", d.exchange);
            ack_pub.publish_ack(correlation_id, d.instrument_id, d.exchange.c_str(), AckStatus::NOT_FOUND);
            continue;
        }

        auto [it, inserted] = active_.try_emplace(d.instrument_id);
        if (inserted) {
            it->second = SubscribedInstrument{d.instrument_id, d.exchange, d.symbol, d.depth, {}};
        }
        const bool first_wanter = it->second.wanters.empty();
        it->second.wanters.insert(correlation_id);

        if (first_wanter) {
            adapter->subscribe(d.instrument_id, d.symbol, d.depth);
            bpt::common::log::info("SubscriptionManager: subscribed {} ({}) on {} depth={} (refcount 0→1)",
                                   d.instrument_id,
                                   d.symbol,
                                   d.exchange,
                                   d.depth);
        }

        ack_pub.publish_ack(correlation_id, d.instrument_id, d.exchange.c_str(), AckStatus::OK);
    }
}

void SubscriptionManager::apply_batch(bpt::messages::MdSubscribeBatch& msg, messaging::IAckPublisher& ack_pub) {
    // SBE exchange field is fixed Char8 — long names (e.g. "HYPERLIQUID")
    // arrive truncated. Restore the canonical full name. Remove once SBE
    // schema migrates to ExchangeId enum.
    auto canonicalize = [](std::string s) {
        if (s == "HYPERLIQ")
            return std::string("HYPERLIQUID");
        return s;
    };

    std::vector<SubscribeRequest> desired;
    auto& g = msg.instruments();
    while (g.hasNext()) {
        g.next();
        SubscribeRequest req;
        req.instrument_id = g.instrumentId();
        req.exchange = canonicalize(g.getExchangeAsString());
        req.symbol = g.getSymbolAsString();
        req.depth = g.depth();
        desired.push_back(std::move(req));
    }

    apply_requests(msg.correlationId(), desired, ack_pub);
}

void SubscriptionManager::publish_subscription_heartbeats(messaging::IAckPublisher& ack_pub) {
    for (const auto& [id, _] : active_) {
        ack_pub.publish_subscription_heartbeat(id);
    }
}

void SubscriptionManager::stop_all() {
    for (auto& a : adapters_)
        a->stop();
}

}  // namespace bpt::md_gateway::subscription
