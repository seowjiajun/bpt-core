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

// Parses OKX WebSocket order-acknowledgement and orders-channel events into
// ExecEvents.  Owns the cloid→order_id map and the duplicate-suppression sets
// for ACKED/CANCELLED orders.
class OKXExecParser {
public:
    std::function<void(const ExecEvent&)> on_exec_event;

    // Register a client order ID before the order is sent.
    void register_order(const std::string& cloid, uint64_t order_id);

    // Provide the instId→ctVal map fetched by fetch_inst_id_codes() so the
    // parser can convert OKX contract sizes to Strategy base-currency quantities.
    void set_contract_sizes(const std::unordered_map<std::string, double>& sizes);

    // Clear duplicate-suppression sets on reconnect.
    void reset();

    // Called for each element in the "data" array of an op=="order" response.
    void handle_order_ack(const boost::json::object& d, uint64_t recv_ns);

    // Called for each element in the "data" array of the "orders" channel push.
    void handle_orders_channel_item(const boost::json::object& d, uint64_t recv_ns);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, uint64_t> cloid_to_order_id_;
    std::unordered_set<uint64_t> acked_orders_;
    std::unordered_set<uint64_t> cancelled_orders_;
    std::unordered_map<std::string, double> contract_sizes_;
};

}  // namespace bpt::order_gateway::adapter
