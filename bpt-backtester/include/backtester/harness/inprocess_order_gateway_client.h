#pragma once

/// \file
/// \brief InProcessOrderGatewayClient — synchronous bridge from Strategy to MatchingEngine.
///
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
#include "backtester/results/results_collector.h"
#include "strategy/order/i_order_gateway_client.h"

#include <messages/AccountSnapshot.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/ExecutionReport.h>
#include <messages/OrderGatewayHeartbeat.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectSource.h>
#include <messages/TimeInForce.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace bpt::backtester::harness {

class InProcessOrderGatewayClient : public bpt::strategy::order::IOrderGatewayClient {
public:
    /// \param matching shared engine the harness drives with market
    ///                 events; submit/cancel route through it.
    explicit InProcessOrderGatewayClient(matching::MatchingEngine& matching);

    /// Wire a ResultsCollector to receive each FillReport. Called by the
    /// harness after both objects are constructed (results_ depends on
    /// strategy_cfg_ which is loaded later, so we can't pass it through
    /// the ctor).
    void set_results_collector(results::ResultsCollector* results) noexcept { results_ = results; }

    [[nodiscard]] bool send_new_order(const bpt::strategy::order::OutboundNewOrder& order) override;

    void send_cancel(const bpt::strategy::order::CancelOrderRequest& cancel) override;

    void send_cancel_all(bpt::messages::ExchangeId::Value exchange_id, uint64_t instrument_id) override;

    void send_modify(const bpt::strategy::order::ModifyOrderRequest& modify) override;

    void send_account_snapshot_request(bpt::messages::ExchangeId::Value exchange_id, uint64_t correlation_id) override;

    /// \brief Push-driven by the harness — events arrive synchronously.
    int poll(int /*fragment_limit*/ = 10) override { return 0; }

    [[nodiscard]] uint64_t last_heartbeat_ns() const override { return last_heartbeat_ns_; }

    /// \name Inbound event callbacks
    /// The base `IOrderGatewayClient` dropped its `std::function` callback
    /// fields in the LMAX CRTP refactor (live path uses templated dispatch
    /// into `StrategyService` now). The backtester harness is slow-path
    /// and benefits from the lambda flexibility, so the concrete
    /// `InProcessOrderGatewayClient` keeps its own callback fields here.
    /// Wired by `StrategyHarness` to forward into the strategy.
    /// @{
    std::function<void(const bpt::messages::ExecutionReport&)> on_exec_report;
    std::function<void(const bpt::messages::OrderGatewayHeartbeat&)> on_heartbeat;
    std::function<void(bpt::messages::AccountSnapshot&)> on_account_snapshot;
    /// @}

    /// \brief Harness-side. Called when the simulated time advances; updates
    ///        the timestamps the strategy's liveness watchdog reads.
    void set_simulation_time(uint64_t now_ns);

    /// \brief Harness-side. Called once per heartbeat tick to fire on_heartbeat
    ///        at the simulated cadence (not wallclock).
    void push_heartbeat();

    /// \brief Harness-side. Called by the harness's tick loop after a market
    ///        event has been ingested by the matching engine — gives the
    ///        engine a chance to fill any pending LIMITs that just became
    ///        crossable.
    ///
    /// The matching engine fires its FillCallback (which this client owns)
    /// as part of that fill loop.
    void on_market_event_complete();

private:
    /// \brief Per-tracked-order metadata.
    ///
    /// Strategy emits orders by uint64 order_id; matching engine identifies
    /// them by string. We keep a forward + reverse map so both directions
    /// are O(1).
    struct LiveOrder {
        uint64_t strategy_order_id;
        bpt::messages::ExchangeId::Value exchange_id;
        uint64_t instrument_id;
        bpt::messages::OrderSide::Value side;
        bpt::messages::OrderType::Value order_type;
        bpt::messages::TimeInForce::Value tif;
        int64_t price;      ///< strategy-side scaled.
        uint64_t quantity;  ///< strategy-side scaled.
        std::string exchange_symbol;
        std::string exchange;               ///< upper-case e.g. "HYPERLIQUID".
        uint64_t cumulative_filled_qty{0};  ///< scaled.
        bool post_only{false};
    };

    /// \brief Translate strategy's ExchangeId enum to the upper-case string
    ///        MatchingEngine keys orders by.
    static std::string exchange_id_string(bpt::messages::ExchangeId::Value id);

    /// \brief Convert a MatchingEngine FillReport to the SBE ExecutionReport
    ///        the strategy expects, then fire on_exec_report.
    ///
    /// Updates the LiveOrder's cumulative_filled_qty so successive partial
    /// fills produce a coherent FILLED status sequence.
    void publish_fill(const matching::FillReport& fr);

    /// \brief One ExecutionReport to synthesise (fill or non-fill). Bundled
    ///        so the 4 emit sites can't transpose the four uint64 fields.
    struct ExecStatusEvent {
        uint64_t order_id;
        bpt::messages::ExchangeId::Value exchange_id;
        uint64_t instrument_id;
        bpt::messages::ExecStatus::Value status;
        bpt::messages::OrderSide::Value side;
        bpt::messages::OrderType::Value order_type;
        int64_t price;
        uint64_t quantity;
        uint64_t cumulative_filled_qty;
        bpt::messages::RejectSource::Value reject_source = bpt::messages::RejectSource::EXCHANGE;
    };

    /// \brief Build + fire an ExecutionReport (ACKED, CANCELLED, REJECTED,
    ///        FILLED, PARTIAL). Drives the strategy's order-state machine.
    void publish_exec_status(const ExecStatusEvent& ev);

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

    /// Optional — when set, every FillReport from the matching engine is
    /// also forwarded to the results collector so summary.json / trades.csv
    /// reflect the run's P&L. Owned by the harness; nullptr is valid
    /// (unit tests that don't need result aggregation).
    results::ResultsCollector* results_{nullptr};
};

}  // namespace bpt::backtester::harness
