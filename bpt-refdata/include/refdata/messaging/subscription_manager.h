#pragma once

#include "refdata/messaging/messages.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bpt::refdata::messaging {

// Tracks which clients have active subscriptions (keyed by correlation_id).
// Only accessed from the poll thread — no locking required.
struct SubscriptionFilter {
    uint64_t correlation_id;
    std::vector<InstrumentFilter> instruments;
};

class SubscriptionManager {
public:
    void upsert(const RefdataRequest& request);
    void remove(uint64_t correlation_id);
    bool has_subscription(uint64_t correlation_id) const;

    const std::unordered_map<uint64_t, SubscriptionFilter>& all() const { return subscriptions_; }

private:
    std::unordered_map<uint64_t, SubscriptionFilter> subscriptions_;
};

}  // namespace bpt::refdata::messaging
