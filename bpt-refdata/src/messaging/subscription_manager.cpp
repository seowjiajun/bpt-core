#include "refdata/messaging/subscription_manager.h"

#include <bpt_common/logging.h>

namespace bpt::refdata::messaging {

void SubscriptionManager::upsert(const RefdataRequest& request) {
    bool is_new = subscriptions_.find(request.correlation_id) == subscriptions_.end();

    SubscriptionFilter filter;
    filter.correlation_id = request.correlation_id;
    filter.instruments = request.instruments;
    subscriptions_[request.correlation_id] = filter;

    if (is_new) {
        bpt::common::log::info("New subscription: correlation_id={} filters={}",
                               request.correlation_id,
                               request.instruments.size());
    } else {
        bpt::common::log::debug("Refreshed subscription: correlation_id={}", request.correlation_id);
    }
}

void SubscriptionManager::remove(uint64_t correlation_id) {
    if (subscriptions_.erase(correlation_id)) {
        bpt::common::log::info("Removed subscription: correlation_id={}", correlation_id);
    }
}

bool SubscriptionManager::has_subscription(uint64_t correlation_id) const {
    return subscriptions_.count(correlation_id) > 0;
}

}  // namespace bpt::refdata::messaging
