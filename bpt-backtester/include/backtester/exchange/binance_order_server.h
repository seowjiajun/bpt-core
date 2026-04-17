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

class BinanceOrderSession;

// Mock Binance order REST + user-data-stream WS server for backtesting.
//
// Listens on a configurable port and implements the subset of the Binance order
// API that Heimdall's BinanceAdapter uses:
//
//   POST   /api/v3/userDataStream  → {"listenKey":"backtester-stream"}
//   PUT    /api/v3/userDataStream  → 200 OK (keepalive)
//   DELETE /api/v3/userDataStream  → 200 OK
//   POST   /api/v3/order           → place order, returns Binance order JSON
//   DELETE /api/v3/order           → cancel order
//   WS     /ws/backtester-stream  → user data stream (executionReport events)
class BinanceOrderServer {
public:
    BinanceOrderServer(uint16_t port, matching::MatchingEngine& engine);
    ~BinanceOrderServer();

    void start();
    void stop();

    // Thread-safe: push an execution report to all connected user-data WS sessions.
    void push_fill(const matching::FillReport& fill);

private:
    void do_accept();

    uint16_t port_;
    matching::MatchingEngine& engine_;
    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<BinanceOrderSession>> sessions_;
    std::atomic<uint64_t> order_id_seq_{0};
    std::thread thread_;
};

}  // namespace bpt::backtester::exchange
