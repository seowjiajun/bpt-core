#pragma once

/// @file
/// IMdClient — abstract interface for the strategy's market-data client.
///
/// Two implementations live behind this interface:
///
///   AeronMdClient  — production path. Publishes MdSubscribeBatch on the
///                    control stream and consumes MdMarketData / MdTrade /
///                    MdOrderBook from the data stream over Aeron IPC.
///                    Today's strategy_app uses this for both live trading
///                    and the multi-process backtest stack.
///
///   InProcessMdClient — deterministic backtest path. Driven directly off
///                       captured tape; no Aeron, no thread, no queue.
///                       Used by the single-process strategy harness so
///                       parameter sweeps produce bit-identical summaries.
///
/// Both implement the same public surface so AvellanedaStoikovStrategy
/// (and every other strategy) is identical bytes regardless of which
/// transport sits behind. Keeping the std::function callback fields as
/// public data members preserves the existing assignment idiom
/// (`client->on_bbo = lambda;`) — only the polymorphic methods are
/// virtual.

#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bpt::strategy::md {

class IMdClient {
public:
    /// Per-instrument subscription request payload. Strategy assembles one
    /// of these per quoted instrument before calling subscribe().
    struct InstrumentDesc {
        uint64_t instrument_id;
        std::string exchange;  // e.g. "BINANCE"
        std::string symbol;    // exchange-native symbol, e.g. "BTCUSDT"
        uint8_t depth{0};      // 0 = BBO only, 5 = top-5 order book levels
    };

    using OnBboFn = std::function<void(const bpt::messages::MdMarketData&)>;
    using OnTradeFn = std::function<void(const bpt::messages::MdTrade&)>;
    using OnOrderBookFn = std::function<void(const bpt::messages::MdOrderBook&)>;
    using OnServiceHeartbeatFn = std::function<void()>;

    virtual ~IMdClient() = default;

    /// Send a full-replace subscription batch.
    virtual void subscribe(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) = 0;

    /// Poll both data and ack/hb streams. Returns total fragment count.
    /// Backtest impls return 0 (no polling needed; events are pushed).
    virtual int poll(int fragment_limit = 10) = 0;

    /// Fired for each BBO tick received from the MD service.
    /// Mutually exclusive with on_order_book — MdGateway publishes one or
    /// the other depending on its order_book_depth config (0 = BBO,
    /// N > 0 = order book).
    OnBboFn on_bbo;

    /// Fired for each trade tick received from the MD service.
    OnTradeFn on_trade;

    /// Fired for each order book snapshot received from the MD service.
    /// Only populated when MdGateway is configured with order_book_depth > 0.
    OnOrderBookFn on_order_book;

    /// Fired each time a MdServiceHeartbeat is received from MdGateway.
    /// Used by StrategyApp to track local receipt time for the liveness watchdog.
    OnServiceHeartbeatFn on_service_heartbeat;
};

}  // namespace bpt::strategy::md
