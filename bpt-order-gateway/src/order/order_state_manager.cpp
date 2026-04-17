#include "order_gateway/order/order_state_manager.h"

namespace bpt::order_gateway::order {

static bool is_open(OrderLifecycle lc) {
    return lc == OrderLifecycle::PENDING || lc == OrderLifecycle::ACKED || lc == OrderLifecycle::PARTIALLY_FILLED;
}

bool OrderStateManager::insert(const OrderState& state) {
    auto [it, inserted] = orders_.emplace(state.order_id, state);
    if (inserted && is_open(state.lifecycle))
        ++total_open_;
    return inserted;
}

const OrderState* OrderStateManager::get(uint64_t order_id) const {
    auto it = orders_.find(order_id);
    if (it == orders_.end())
        return nullptr;
    return &it->second;
}

OrderState* OrderStateManager::get_mut(uint64_t order_id) {
    auto it = orders_.find(order_id);
    if (it == orders_.end())
        return nullptr;
    return &it->second;
}

bool OrderStateManager::update(uint64_t order_id,
                               OrderLifecycle new_lifecycle,
                               uint64_t exchange_order_id,
                               uint64_t filled_qty,
                               uint64_t remaining_qty,
                               uint64_t timestamp_ns) {
    auto it = orders_.find(order_id);
    if (it == orders_.end())
        return false;

    OrderState& state = it->second;
    bool was_open = is_open(state.lifecycle);
    state.lifecycle = new_lifecycle;
    bool now_open = is_open(new_lifecycle);
    if (was_open && !now_open)
        --total_open_;
    else if (!was_open && now_open)
        ++total_open_;
    if (exchange_order_id != 0)
        state.exchange_order_id = exchange_order_id;
    state.filled_qty = filled_qty;
    state.remaining_qty = remaining_qty;
    state.last_update_ns = timestamp_ns;
    return true;
}

void OrderStateManager::remove(uint64_t order_id) {
    auto it = orders_.find(order_id);
    if (it == orders_.end())
        return;
    if (is_open(it->second.lifecycle))
        --total_open_;
    orders_.erase(it);
}

uint32_t OrderStateManager::open_order_count(bpt::messages::ExchangeId::Value exchange) const {
    uint32_t count = 0;
    for (const auto& [id, state] : orders_) {
        if (state.exchange_id == exchange &&
            (state.lifecycle == OrderLifecycle::PENDING || state.lifecycle == OrderLifecycle::ACKED ||
             state.lifecycle == OrderLifecycle::PARTIALLY_FILLED)) {
            ++count;
        }
    }
    return count;
}

}  // namespace bpt::order_gateway::order
