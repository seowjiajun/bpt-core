#pragma once

/// @file
/// IMdClient — minimal abstract interface exposing the methods strategies
/// call on the MD client (`subscribe`, `poll`). The per-frame dispatch
/// path was lifted to a CRTP-templated concrete client — see
/// `md_client.h` (`AeronMdClient<Handler>`) and `inprocess_md_client.h`
/// (`InProcessMdClient<Handler>`). Each is parameterised on a Handler
/// type with `on_bbo`, `on_trade`, `on_order_book`, `on_md_service_heartbeat`
/// methods (in prod the Handler is `StrategyService`). Removing the
/// `std::function` callback fields kills one level of indirection per
/// tick — same shape used by `bpt-md-gateway` for its decoder→publisher
/// chain.
///
/// Two implementations live behind this interface:
///
///   AeronMdClient<Handler>     — production path; Aeron IPC.
///   InProcessMdClient<Handler> — deterministic backtest harness.
///
/// Strategies keep `IMdClient*` (this interface) for the subscribe/poll
/// surface so they don't have to know which Handler the concrete client
/// is templated on.

#include <cstdint>
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

    virtual ~IMdClient() = default;

    /// Send a full-replace subscription batch.
    virtual void subscribe(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) = 0;

    /// Poll both data and ack/hb streams. Returns total fragment count.
    /// Backtest impls return 0 (no polling needed; events are pushed).
    virtual int poll(int fragment_limit = 10) = 0;
};

}  // namespace bpt::strategy::md
