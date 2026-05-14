#pragma once

/// \file
/// \brief Tracks active subscriptions per consumer; refcounts at the venue.
///
/// Multiple services can independently drive MD subscriptions (e.g. strategy
/// wants a few trading pairs, pricer wants front-month option strikes, future
/// radar modules may want perp/spot for cross-asset signals). Each consumer
/// owns its own "desired set" of subscriptions keyed by the correlation_id
/// it stamps on every MdSubscribeBatch it sends. The manager unions across
/// consumers when deciding what to ask the venue adapters for: an instrument
/// is subscribed once at the adapter as soon as ANY consumer wants it, and
/// unsubscribed only when the LAST consumer drops it.
///
/// Semantics per incoming batch:
///   * Per-consumer state: that correlation_id's desired set is REPLACED by
///     the batch. Consumers send the full universe they want each time;
///     incremental subscribe/unsubscribe is not supported. Same shape as
///     before, just scoped per consumer.
///   * Global state: the union across consumers is recomputed; adapter
///     subscribe is invoked only on union 0→1 transitions, unsubscribe on
///     union 1→0. No duplicate venue subscriptions, no over-eager unsubs.
///
/// One ack per instrument in the incoming batch (sent to the originating
/// correlation_id only — other consumers don't see them).

#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/messaging/i_ack_publisher.h"

#include <messages/MdSubscribeBatch.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bpt::md_gateway::subscription {

class SubscriptionManager {
public:
    struct SubscribeRequest {
        uint64_t instrument_id;
        std::string exchange;
        std::string symbol;
        uint8_t depth{0};
    };

    void add_adapter(std::shared_ptr<adapter::IAdapter> adapter);

    /// \brief Apply a full-replace desired set scoped to `correlation_id`.
    ///
    /// Recomputes the global union; subscribes/unsubscribes the venue only
    /// for instruments whose refcount transitions 0↔1. Sends one ack per
    /// instrument in `desired`.
    void apply_requests(uint64_t correlation_id,
                        const std::vector<SubscribeRequest>& desired,
                        messaging::IAckPublisher& ack_pub);

    void apply_batch(bpt::messages::MdSubscribeBatch& msg, messaging::IAckPublisher& ack_pub);

    void publish_subscription_heartbeats(messaging::IAckPublisher& ack_pub);

    void stop_all();

    /// \brief Number of distinct instruments currently subscribed at the venue.
    ///
    /// Counts union members, not consumer-side desires. If three consumers
    /// each want the same instrument, this returns 1.
    [[nodiscard]] std::size_t active_count() const { return active_.size(); }

private:
    struct SubscribedInstrument {
        uint64_t instrument_id;
        std::string exchange;
        std::string symbol;
        uint8_t depth{0};
        std::unordered_set<uint64_t> wanters;  ///< correlation_ids that requested this
    };

    std::vector<std::shared_ptr<adapter::IAdapter>> adapters_;

    /// instrument_id → global subscription state (union of consumer wants).
    std::unordered_map<uint64_t, SubscribedInstrument> active_;

    /// Per-consumer desired set: correlation_id → {instrument_ids it wants}.
    /// Used so a consumer's replace-batch only affects its own wants, not
    /// other consumers'.
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> per_consumer_;

    adapter::IAdapter* find_adapter(const std::string& exchange);
};

}  // namespace bpt::md_gateway::subscription
