#pragma once

/// \file
/// \brief Tracks active subscriptions and routes them to the right adapter.
///
/// One SubscriptionManager per gateway process. Holds the canonical
/// "currently subscribed" set; each incoming MdSubscribeBatch (or the
/// startup standalone path) is treated as a full-replace of that set.
/// The manager diffs against `active_`, dispatches subscribe/unsubscribe
/// calls to the per-venue adapters, and emits acks back to strategy.

#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/messaging/i_ack_publisher.h"

#include <messages/MdSubscribeBatch.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::md_gateway::subscription {

/// \brief Diff-based subscription dispatcher.
///
/// Single-threaded — only call from MdGatewayService's main poll thread.
/// Cross-thread interaction with the per-venue adapters happens
/// internally inside each adapter's own subscribe/unsubscribe
/// implementation; the manager itself never spawns threads.
///
/// Full-state-replace semantic: every incoming `MdSubscribeBatch` is
/// the complete desired set. Unsubscribes are inferred (an instrument
/// that was active but is missing from the new batch). One
/// MdSubscriptionAck is emitted per instrument in the new batch.
class SubscriptionManager {
public:
    /// \brief One requested subscription.
    ///
    /// Produced either by decoding an incoming MdSubscribeBatch or by
    /// the standalone startup path that loads a fixed universe from
    /// config.
    struct SubscribeRequest {
        uint64_t instrument_id;
        std::string exchange;
        std::string symbol;
        uint8_t depth{0};
    };

    /// \brief Register an adapter. Multiple adapters may serve different exchanges.
    void add_adapter(std::shared_ptr<adapter::IAdapter> adapter);

    /// \brief Apply a full-replace desired subscription set.
    ///
    /// Diffs against `active_`, unsubscribes what dropped out,
    /// subscribes what's new, acks everything still desired. Used by
    /// both `apply_batch()` (the SBE control stream) and the standalone
    /// startup path.
    void apply_requests(uint64_t correlation_id,
                        const std::vector<SubscribeRequest>& desired,
                        messaging::IAckPublisher& ack_pub);

    /// \brief Apply a control-stream batch.
    ///
    /// Sends acks via `ack_pub` for every instrument in the batch.
    void apply_batch(bpt::messages::MdSubscribeBatch& msg, messaging::IAckPublisher& ack_pub);

    /// \brief Publish per-instrument heartbeats for all currently active subscriptions.
    void publish_subscription_heartbeats(messaging::IAckPublisher& ack_pub);

    /// \brief Stop every registered adapter and join their threads.
    ///
    /// Call once after the main poll loop exits.
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
    std::unordered_map<uint64_t, SubscribedInstrument> active_;  ///< instrument_id → SubscribedInstrument

    adapter::IAdapter* find_adapter(const std::string& exchange);
};

}  // namespace bpt::md_gateway::subscription
