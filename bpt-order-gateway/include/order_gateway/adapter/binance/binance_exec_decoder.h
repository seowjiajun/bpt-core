#pragma once

#include "order_gateway/adapter/common/i_order_adapter.h"

#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <boost/json.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace bpt::order_gateway::adapter {

// Parses Binance executionReport WebSocket events into ExecEvents.
// Owns the cloid→order_id map so both the WS handler and send_new_order
// can register/look up client order IDs without locking from two places.
class BinanceExecDecoder {
public:
    std::function<void(const ExecEvent&)> on_exec_event;

    // Register a client order ID before the order is sent.
    void register_order(const std::string& cloid, uint64_t order_id);

    // Called for each "executionReport" event on the user-data WebSocket.
    void handle_execution_report(const boost::json::object& obj, uint64_t recv_ns);

    // Called with the parsed JSON body of a POST /api/v3/order response.
    // Emits one ExecEvent (ACK/FILLED/PARTIAL/REJECTED). `order_id`,
    // `side`, and `order_type` come from the original NewOrder because
    // the REST response carries them in string form but the adapter
    // already has them typed. Also emits a REJECTED event if the JSON
    // has a top-level `code` field (Binance's error shape).
    void handle_order_response(const boost::json::object& obj,
                                uint64_t order_id,
                                bpt::messages::OrderSide::Value side,
                                bpt::messages::OrderType::Value order_type,
                                uint64_t recv_ns);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, uint64_t> cloid_to_order_id_;
};

}  // namespace bpt::order_gateway::adapter
