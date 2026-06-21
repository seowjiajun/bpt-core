#pragma once

/// \file
/// \brief IOrderAdapter — the per-venue order-routing interface used by order-gateway.

#include "order_gateway/adapter/common/account_snapshot_data.h"
#include "order_gateway/messaging/publishers/api/exec_report_publisher.h"
#include "order_gateway/order/inbound_order_events.h"
#include "order_gateway/order/order_state_manager.h"
#include "order_gateway/risk/disconnect_rate_breaker.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <bpt_common/util/topology.h>
#include <cstdint>
#include <functional>
#include <string>

namespace bpt::order_gateway::adapter {

/// \brief Exec event fired by adapters on every exchange report (ack, fill, cancel, reject).
///
/// All fields use the same fixed-point scale as the SBE messages
/// (1e8 for price/qty).
struct ExecEvent {
    uint64_t order_id{0};
    uint64_t exchange_order_id{0};
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id{0};
    bpt::messages::ExecStatus::Value status;
    bpt::messages::OrderSide::Value side;
    bpt::messages::OrderType::Value order_type;
    int64_t price{0};
    uint64_t filled_qty{0};
    uint64_t remaining_qty{0};
    bpt::messages::RejectReason::Value reject_reason;
    int64_t fee{0};
    /// Currency the fee is charged in, as the venue reported it (e.g.
    /// "USDT", "USDC", "BTC", "OKB"). Verbatim from the venue's wire
    /// field; no enum mapping. Up to 8 chars (encoded into the SBE
    /// ExecutionReport's Char8 feeCurrency slot, zero-padded).
    std::string fee_currency;
    uint64_t exchange_ts_ns{0};
    uint64_t local_ts_ns{0};

    /// \brief Project this event onto the strategy-facing exec-report DTO
    /// (fields map 1:1). The adapter owns the conversion — outer layer
    /// depending on the messaging port, the correct hexagonal direction.
    [[nodiscard]] messaging::api::ExecReport to_report() const {
        return messaging::api::ExecReport{
            .order_id = order_id,
            .exchange_order_id = exchange_order_id,
            .exchange_id = exchange_id,
            .instrument_id = instrument_id,
            .status = status,
            .side = side,
            .order_type = order_type,
            .price = price,
            .filled_qty = filled_qty,
            .remaining_qty = remaining_qty,
            .reject_reason = reject_reason,
            .fee = fee,
            .fee_currency = fee_currency,
            .exchange_ts_ns = exchange_ts_ns,
            .local_ts_ns = local_ts_ns,
        };
    }
};

class IOrderAdapter {
public:
    virtual ~IOrderAdapter() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    /// \name Order entry
    /// Non-blocking: each call extracts the SBE fields onto a value-typed
    /// `SendWorkItem` and pushes onto the per-adapter send-work queue.
    /// The send-executor thread drains the queue and invokes the
    /// venue-specific blocking work (HTTPS POST / WS send + future wait).
    /// Resulting `ExecEvent`s are delivered via `drain_exec_events()`.
    /// Called from the main poll thread.
    /// @{
    virtual void send_new_order(const order::NewOrderEvent& order) = 0;
    virtual void send_cancel(const order::CancelOrderEvent& cancel, const std::string& native_symbol) = 0;
    virtual void send_cancel_all(uint64_t instrument_id) = 0;
    virtual void send_modify(const order::ModifyOrderEvent& modify, const std::string& native_symbol) = 0;
    /// @}

    [[nodiscard]] virtual bpt::messages::ExchangeId::Value exchange_id() const = 0;
    [[nodiscard]] virtual const char* exchange_name() const = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;

    /// \brief Disconnect-rate circuit breaker.
    ///
    /// Latches true when the adapter has reconnected too many times in
    /// its rolling window — operator restart required to clear.
    /// Distinct from `!is_connected()` (which is a transient state
    /// during normal reconnect). Default false for adapters that don't
    /// implement it.
    [[nodiscard]] virtual bool is_halted() const { return false; }

    /// \brief Set disconnect-breaker config before start().
    ///
    /// Default no-op; OrderAdapterBase overrides.
    virtual void set_disconnect_breaker_config(risk::DisconnectRateBreaker::Config) {}

    /// \brief Bind the central CPU-affinity topology.
    ///
    /// Must be called before start() — the IO thread reads its role
    /// assignment at launch. Default no-op; OrderAdapterBase overrides.
    virtual void set_topology(const bpt::common::util::Topology&) {}

    /// \brief Drain all pending exec events from the adapter's IO thread.
    ///
    /// Call this from the main poll loop on every iteration. `fn` is
    /// invoked once per event.
    /// \return The number of events drained.
    virtual int drain_exec_events(const std::function<void(const ExecEvent&)>& fn) = 0;

    /// \brief Fetch current account positions and balance from the exchange REST API.
    ///
    /// Blocking — runs on the `AccountSnapExecutor` thread, not the poll
    /// loop.
    /// \return A populated AccountSnapshotData.
    /// \throws std::exception on failure.
    virtual AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) = 0;
};

}  // namespace bpt::order_gateway::adapter
