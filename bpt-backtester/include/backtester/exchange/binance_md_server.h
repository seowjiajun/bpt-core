#pragma once

#include "backtester/data/market_event.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace bpt::backtester::exchange {

class BinanceSession;

// Mock Binance WebSocket MD server for backtesting.
//
// Listens on a configurable port and serves Binance-format combined-stream
// WebSocket messages (same protocol as stream.binance.com:9443/stream).
// Huginn's BinanceAdapter connects here instead of the real exchange when
// backtest_mode is active.
//
// Usage:
//   BinanceMdServer srv(9100);
//   srv.start();
//   // ... in event loop:
//   srv.push(market_event);
//   srv.stop();
class BinanceMdServer {
public:
    explicit BinanceMdServer(uint16_t port);
    ~BinanceMdServer();

    void start();
    void stop();

    // Thread-safe: deliver a market event to all subscribed sessions.
    // Formats the event as Binance combined-stream JSON and dispatches to
    // every session whose subscription set covers the event's symbol/type.
    void push(const data::MarketEvent& event);

private:
    void do_accept();

    uint16_t port_;
    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<BinanceSession>> sessions_;
    uint64_t trade_id_{0};
    uint64_t update_id_{0};
    std::thread thread_;
};

}  // namespace bpt::backtester::exchange
