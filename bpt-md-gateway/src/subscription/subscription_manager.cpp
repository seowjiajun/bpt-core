#include "md_gateway/subscription/subscription_manager.h"

#include <messages/AckStatus.h>
#include <messages/MessageHeader.h>

#include <cstring>
#include <unordered_set>
#include <bpt_common/logging.h>

namespace bpt::md_gateway::subscription {

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

void SubscriptionManager::apply_batch(bpt::messages::MdSubscribeBatch& msg, messaging::IAckPublisher& ack_pub) {
    using namespace bpt::messages;

    uint64_t correlation_id = msg.correlationId();

    // Collect the new desired set
    struct Instrument {
        uint64_t id;
        std::string exchange;
        std::string symbol;
        uint8_t depth{0};
    };
    std::vector<Instrument> desired;

    // The SBE exchange field is a fixed 8-char array, so long names like
    // "HYPERLIQUID" arrive truncated to "HYPERLIQ". Restore the canonical
    // full name to match the adapter registry. Remove once SBE schema is
    // widened / migrated to ExchangeId enum.
    auto canonicalize = [](std::string s) {
        if (s == "HYPERLIQ") return std::string("HYPERLIQUID");
        return s;
    };

    auto& g = msg.instruments();
    while (g.hasNext()) {
        g.next();
        Instrument inst;
        inst.id = g.instrumentId();
        inst.exchange = canonicalize(g.getExchangeAsString());
        inst.symbol = g.getSymbolAsString();
        inst.depth = g.depth();
        desired.push_back(std::move(inst));
    }

    // Compute to_remove = active - desired
    std::unordered_set<uint64_t> desired_ids;
    for (const auto& d : desired)
        desired_ids.insert(d.id);

    for (auto it = active_.begin(); it != active_.end();) {
        if (desired_ids.find(it->first) == desired_ids.end()) {
            adapter::IAdapter* adapter = find_adapter(it->second.exchange);
            if (adapter)
                adapter->unsubscribe(it->first);
            bpt::common::log::info("SubscriptionManager: unsubscribed {} on {}", it->first, it->second.exchange);
            it = active_.erase(it);
        } else {
            ++it;
        }
    }

    // Compute to_add = desired - active; send ack for all in desired
    for (const auto& d : desired) {
        adapter::IAdapter* adapter = find_adapter(d.exchange);

        if (!adapter) {
            bpt::common::log::warn("SubscriptionManager: no adapter for exchange {}", d.exchange);
            ack_pub.publish_ack(correlation_id, d.id, d.exchange.c_str(), AckStatus::NOT_FOUND);
            continue;
        }

        if (active_.find(d.id) == active_.end()) {
            adapter->subscribe(d.id, d.symbol, d.depth);
            active_[d.id] = {d.id, d.exchange, d.symbol, d.depth};
            bpt::common::log::info("SubscriptionManager: subscribed {} ({}) on {} depth={}",
                           d.id,
                           d.symbol,
                           d.exchange,
                           d.depth);
        }

        ack_pub.publish_ack(correlation_id, d.id, d.exchange.c_str(), AckStatus::OK);
    }
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
