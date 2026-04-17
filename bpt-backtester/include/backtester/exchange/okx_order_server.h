#pragma once

#include "backtester/matching/matching_engine.h"
#include "backtester/matching/open_order.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace bpt::backtester::exchange {

class OkxOrderSession;

// Mock OKX order WebSocket server for backtesting.
//
// Listens on a configurable port and implements the subset of the OKX order
// API that Heimdall's OKXAdapter uses:
//
//   WS /ws/v5/private  — single bidirectional channel for both order ops and fills
//
// Incoming op messages handled:
//   {"op":"order",  "args":[{...}]}   → place order
//   {"op":"cancel-order","args":[...]} → cancel order
//
// Outgoing push messages:
//   {"arg":{"channel":"orders"},"data":[{...}]}  — fill / status update
class OkxOrderServer {
public:
    OkxOrderServer(uint16_t port, matching::MatchingEngine& engine);
    ~OkxOrderServer();

    void start();
    void stop();

    // Thread-safe: push an execution report to all connected sessions.
    void push_fill(const matching::FillReport& fill);

private:
    void do_accept();

    uint16_t port_;
    matching::MatchingEngine& engine_;
    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<OkxOrderSession>> sessions_;
    std::atomic<uint64_t> order_id_seq_{0};
    std::thread thread_;
};

}  // namespace bpt::backtester::exchange
