#pragma once

/// \file
/// \brief Mock Hyperliquid order WebSocket server for backtesting.

#include "backtester/matching/matching_engine.h"
#include "backtester/matching/open_order.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bpt::backtester::exchange {

class HyperliquidOrderSession;

/// \brief Mock Hyperliquid order WebSocket server for backtesting.
///
/// Implements the subset of HL's WS post-action protocol that
/// OrderGateway's HyperliquidOrderAdapter uses:
///
///   {"method":"post","id":N,"request":{"type":"action",
///    "payload":{"action":{...},"nonce":...,"signature":{...}}}}
///
/// Embedded actions:
///   {"type":"order","orders":[{"a":3,"b":true,"p":"...","s":"...","r":false,
///                              "t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"}
///   {"type":"cancel","cancels":[{"a":3,"o":12345}]}
///
/// Asset index `a` is HL's universe-positional ID. This sim doesn't run a
/// /info REST endpoint, so order-gateway's inst_id_codes map starts empty
/// and all orders arrive with a=0. The simulator takes an asset_universe
/// vector at construction (typically populated from BacktesterService's
/// [[instruments]] list) so a=0 → universe[0] etc.
///
/// Signatures are NOT validated — backtest signers and prod signers diverge
/// and validation would fail. The simulator accepts any signature shape.
///
/// Outgoing:
///   {"channel":"post","data":{"id":N,"response":{"type":"action",
///                              "payload":{"type":"ok","data":{...}}}}}
///   {"channel":"userFills","data":{"isSnapshot":false,"user":"0x...",
///                                  "fills":[{"oid":...,...}]}}
class HyperliquidOrderServer {
public:
    HyperliquidOrderServer(uint16_t port, matching::MatchingEngine& engine, std::vector<std::string> asset_universe);
    ~HyperliquidOrderServer();

    void start();
    void stop();

    void push_fill(const matching::FillReport& fill);

private:
    void do_accept();

    uint16_t port_;
    matching::MatchingEngine& engine_;
    std::vector<std::string> asset_universe_;
    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<HyperliquidOrderSession>> sessions_;
    std::atomic<uint64_t> order_id_seq_{0};
    std::thread thread_;
};

}  // namespace bpt::backtester::exchange
