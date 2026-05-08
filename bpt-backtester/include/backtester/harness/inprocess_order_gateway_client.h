#pragma once

/// @file
/// InProcessOrderGatewayClient — synchronous bridge between
/// bpt-strategy's IOrderGatewayClient and bpt-backtester's MatchingEngine.
/// Replaces the entire Strategy → AeronOGW → OGW → venue-adapter →
/// venue-mock-WS → MatchingEngine path with one inline function call.
///
/// Determinism property: every send_new_order() / send_cancel() call
/// runs MatchingEngine synchronously and fires on_exec_report inline
/// before returning. There is no IO thread, no Aeron, no asio, no WS.
/// When the strategy calls send_new_order on its bid quote, by the time
/// that call returns, the strategy has already received the ACKED
/// ExecutionReport (and any FILLED report if the order crossed at
/// submit time). Pure event-loop semantics — every event fully drains
/// before the next one starts.
///
/// Scope today: matches what AvellanedaStoikovStrategy actually uses.
/// LIMIT GTC + MARKET IOC are first-class. POST_ONLY rejection is
/// honored. Modify is implemented as cancel-replace. Account snapshot
/// requests return a placeholder snapshot from a per-instrument
/// position tracker the harness owns. Heartbeats are synthesised by
/// the harness on a replay-clock cadence.

#include "backtester/matching/matching_engine.h"
#include "backtester/matching/open_order.h"

#include "strategy/order/i_order_gateway_client.h"

#include <messages/AccountSnapshot.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/ExecutionReport.h>
#include <messages/OrderGatewayHeartbeat.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace bpt::backtester::harness {

class InProcessOrderGatewayClient : public bpt::strategy::order::IOrderGatewayClient {
public:
    /// @param matching shared engine the harness drives with market
    ///                 events; submit/cancel route through it.
    explicit InProcessOrderGatewayClient(matching::MatchingEngine& matching);

    [[nodiscard]] bool send_new_order(uint64_t order_id,
                                      bpt::messages::ExchangeId::Value exchange_id,
                                      uint64_t instrument_id,
                                      bpt::messages::OrderSide::Value side,
                                      bpt::messages::OrderType::Value order_type,
                                      bpt::messages::TimeInForce::Value tif,
                                      int64_t price,
                                      uint64_t quantity,
                                      uint8_t exec_inst,
                                      const std::string& exchange_symbol) override;

    void send_cancel(uint64_t order_id,
                     bpt::messages::ExchangeId::Value exchange_id,
                     uint64_t instrument_id) override;

    void send_cancel_all(bpt::messages::ExchangeId::Value exchange_id,
                         uint64_t instrument_id) override;

    void send_modify(uint64_t order_id,
                     bpt::messages::ExchangeId::Value exchange_id,
                     uint64_t instrument_id,
                     int64_t new_price,
                     uint64_t new_quantity) override;

    void send_account_snapshot_request(bpt::messages::ExchangeId::Value exchange_id,
                                        uint64_t correlation_id) override;

    /// Push-driven by the harness — events arrive synchronously.
    int poll(int /*fragment_limit*/ = 10) override { return 0; }

    [[nodiscard]] uint64_t last_heartbeat_ns() const override { return last_heartbeat_ns_; }

    /// Harness-side. Called when the simulated time advances; updates
    /// the timestamps the strategy's liveness watchdog reads.
    void set_simulation_time(uint64_t now_ns);

    /// Harness-side. Called once per heartbeat tick to fire on_heartbeat
    /// at the simulated cadence (not wallclock).
    void push_heartbeat();

    /// Harness-side. Called by the harness's tick loop after a market
    /// event has been ingested by the matching engine — gives the
    /// engine a chance to fill any pending LIMITs that just became
    /// crossable. The matching engine fires its FillCallback (which
    /// this client owns) as part of that fill loop.
    void on_market_event_complete();

private:
    /// Per-tracked-order metadata. Strategy emits orders by uint64
    /// order_id; matching engine identifies them by string. We keep
    /// a forward + reverse map so both directions are O(1).
    struct LiveOrder {
        uint64_t                         strategy_order_id;
        bpt::messages::ExchangeId::Value exchange_id;
        uint64_t                         instrument_id;
        bpt::messages::OrderSide::Value  side;
        bpt::messages::OrderType::Value  order_type;
        bpt::messages::TimeInForce::Value tif;
        int64_t                          price;       // strategy-side scaled
        uint64_t                         quantity;    // strategy-side scaled
        std::string                      exchange_symbol;
        std::string                      exchange;    // upper-case e.g. "HYPERLIQUID"
        uint64_t                         cumulative_filled_qty{0};  // scaled
        bool                             post_only{false};
    };

    /// Translate strategy's ExchangeId enum to the upper-case string
    /// MatchingEngine keys orders by.
    static std::string exchange_id_string(bpt::messages::ExchangeId::Value id);

    /// Convert a MatchingEngine FillReport to the SBE ExecutionReport
    /// the strategy expects, then fire on_exec_report. Updates the
    /// LiveOrder's cumulative_filled_qty so successive partial fills
    /// produce a coherent FILLED status sequence.
    void publish_fill(const matching::FillReport& fr);

    /// Build + fire a non-fill ExecutionReport (ACKED, CANCELLED,
    /// REJECTED). Drives the strategy's order-state machine for events
    /// that don't involve a fill.
    void publish_exec_status(uint64_t order_id,
                             bpt::messages::ExchangeId::Value exchange_id,
                             uint64_t instrument_id,
                             bpt::messages::ExecStatus::Value status,
                             bpt::messages::OrderSide::Value side,
                             bpt::messages::OrderType::Value order_type,
                             int64_t price,
                             uint64_t quantity,
                             uint64_t cumulative_filled_qty);

    matching::MatchingEngine& matching_;

    /// strategy_order_id → LiveOrder
    std::unordered_map<uint64_t, LiveOrder> live_;

    uint64_t simulation_now_ns_{0};
    uint64_t last_heartbeat_ns_{0};

    /// Monotonic seq for ExecutionReport messages — strategies treat
    /// this as a tie-breaker on duplicates; harness needs to fire
    /// strictly increasing.
    uint64_t exec_seq_{0};

    /// Diagnostic — counts POST_ONLY-would-cross rejections. Useful
    /// for operators investigating why a backtest produced zero fills.
    uint64_t rejected_count_{0};
};

}  // namespace bpt::backtester::harness
