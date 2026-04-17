#pragma once

#include "order_gateway/adapter/common/i_order_adapter.h"

#include <boost/json.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace bpt::order_gateway::adapter {

// Parses Deribit WebSocket execution events into ExecEvents.
// Handles the user.orders.any.raw subscription channel and the inline order
// result embedded in private/buy + private/sell JSON-RPC responses.
class DeribitExecParser {
public:
    std::function<void(const ExecEvent&)> on_exec_event;

    // Register label→order_id before the order is sent.
    void register_order(const std::string& label, uint64_t order_id);

    // Look up the Deribit exchange order_id string for a given internal order_id.
    // Returns empty string if not yet received.
    std::string get_exchange_order_id(uint64_t order_id) const;

    // Clear duplicate-suppression sets and exchange-order-id maps on reconnect.
    void reset();

    // Called for the "data" object in a user.orders.any.raw subscription push.
    void handle_subscription_event(const boost::json::object& d, uint64_t recv_ns);

    // Called for the "result.order" object in a private/buy or private/sell
    // JSON-RPC response (only for terminal states: filled, cancelled, rejected).
    void handle_order_response(const boost::json::object& order_obj, uint64_t recv_ns);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, uint64_t> label_to_order_id_;
    std::unordered_map<std::string, uint64_t> exch_oid_to_order_id_;
    std::unordered_map<uint64_t, std::string> order_id_to_exch_oid_;
    std::unordered_set<uint64_t> acked_orders_;
    std::unordered_set<uint64_t> cancelled_orders_;
};

}  // namespace bpt::order_gateway::adapter
