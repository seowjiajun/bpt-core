#pragma once

#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/messaging/i_ack_publisher.h"

#include <messages/MdSubscribeBatch.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::md_gateway::subscription {

// Tracks active subscriptions and routes subscribe/unsubscribe operations to the
// correct adapter.
//
// Single-threaded: only call from the main Aeron poll thread.
//
// Full-state-replace semantics: each MdSubscribeBatch is the complete desired
// subscription set.  The manager diffs it against the current set and calls
// subscribe/unsubscribe on the affected adapters, then sends one MdSubscriptionAck
// per instrument in the new batch.
class SubscriptionManager {
public:
    // Register an adapter.  Multiple adapters may serve different exchanges.
    void add_adapter(std::shared_ptr<adapter::IAdapter> adapter);

    // Process a full-replace subscription batch.
    // Sends acks via ack_pub for every instrument in the batch.
    void apply_batch(bpt::messages::MdSubscribeBatch& msg, messaging::IAckPublisher& ack_pub);

    // Publish per-instrument subscription heartbeats for all active subscriptions.
    void publish_subscription_heartbeats(messaging::IAckPublisher& ack_pub);

    // Signal all adapters to stop and join their threads.  Call once after the main
    // poll loop exits.
    void stop_all();

    [[nodiscard]] std::size_t active_count() const { return active_.size(); }

private:
    struct SubscribedInstrument {
        uint64_t instrument_id;
        std::string exchange;
        std::string symbol;
        uint8_t depth{0};
    };

    std::vector<std::shared_ptr<adapter::IAdapter>> adapters_;
    // instrument_id → SubscribedInstrument
    std::unordered_map<uint64_t, SubscribedInstrument> active_;

    adapter::IAdapter* find_adapter(const std::string& exchange);
};

}  // namespace bpt::md_gateway::subscription
