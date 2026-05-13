#pragma once

/// \file
/// \brief Mock OKX WebSocket market-data server used in backtest mode.

#include "backtester/data/market_event.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace bpt::backtester::exchange {

class OkxMdSession;

/// \brief Mock OKX WebSocket MD server for backtesting.
///
/// Listens on a configurable port and serves OKX-format WebSocket messages
/// (same protocol as ws.okx.com:8443/ws/v5/public).
/// MdGateway's OKXAdapter connects here instead of the real exchange when
/// backtest_mode is active.
///
/// Supported channels:
///   "trades"  — trade prints  → {"arg":{"channel":"trades","instId":"..."},"data":[{...}]}
///   "books5"  — top-5 book    → {"arg":{"channel":"books5","instId":"..."},"data":[{...}]}
///
/// Usage:
///   OkxMdServer srv(9100);
///   srv.start();
///   // ... in event loop:
///   srv.push(market_event);
///   srv.stop();
class OkxMdServer {
public:
    explicit OkxMdServer(uint16_t port);
    ~OkxMdServer();

    void start();
    void stop();

    /// \brief Thread-safe: deliver a market event to all subscribed sessions.
    ///
    /// Formats the event as OKX WebSocket JSON and dispatches to every session
    /// whose subscription set covers the event's symbol/channel.
    void push(const data::MarketEvent& event);

    /// \brief Returns the number of currently active (non-closed) sessions.
    ///
    /// Thread-safe via io_context post.
    std::size_t session_count();

private:
    void do_accept();

    uint16_t port_;
    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<OkxMdSession>> sessions_;
    std::thread thread_;
};

}  // namespace bpt::backtester::exchange
