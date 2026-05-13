#pragma once

/// \file
/// \brief Mock Hyperliquid WebSocket market-data server used in backtest mode.

#include "backtester/data/market_event.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace bpt::backtester::exchange {

class HyperliquidMdSession;

/// \brief Mock Hyperliquid WebSocket MD server for backtesting.
///
/// Listens on a configurable port and serves Hyperliquid-format WebSocket
/// messages (same protocol as wss://api.hyperliquid.xyz/ws). MdGateway's
/// HyperliquidMdAdapter connects here in backtest mode.
///
/// Supported subscription types:
///   "l2Book"          → {"channel":"l2Book","data":{"coin":"...","time":...,"levels":[[bids],[asks]]}}
///   "trades"          → {"channel":"trades","data":[{"coin":"...","px":"...","sz":"...","side":"B"|"A","time":...}]}
///   "activeAssetCtx"  → accepted but not emitted (funding/OI updates not modeled in backtest)
///
/// Mirrors OkxMdServer structure exactly; only the WS protocol differs.
class HyperliquidMdServer {
public:
    explicit HyperliquidMdServer(uint16_t port);
    ~HyperliquidMdServer();

    void start();
    void stop();

    /// \brief Thread-safe: deliver a market event to all subscribed sessions.
    void push(const data::MarketEvent& event);

    std::size_t session_count();

private:
    void do_accept();

    uint16_t port_;
    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<HyperliquidMdSession>> sessions_;
    std::thread thread_;
};

}  // namespace bpt::backtester::exchange
