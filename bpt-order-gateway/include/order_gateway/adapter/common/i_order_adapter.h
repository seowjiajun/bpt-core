#pragma once

#include "order_gateway/adapter/common/account_snapshot_data.h"
#include "order_gateway/order/order_state_manager.h"

#include <messages/CancelOrder.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/ModifyOrder.h>
#include <messages/NewOrder.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <cstdint>
#include <functional>
#include <string>

namespace bpt::order_gateway::adapter {

// Exec event fired by adapters on every exchange report (ack, fill, cancel, reject).
// All fields use the same fixed-point scale as the SBE messages (1e8 for price/qty).
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
    bpt::messages::FeeCurrency::Value fee_currency;
    uint64_t exchange_ts_ns{0};
    uint64_t local_ts_ns{0};
};

class IOrderAdapter {
public:
    virtual ~IOrderAdapter() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    // Place, cancel, modify orders.  Thread-safe — called from the hot-path thread.
    virtual void send_new_order(const bpt::messages::NewOrder& order) = 0;
    virtual void send_cancel(const bpt::messages::CancelOrder& cancel, const std::string& native_symbol) = 0;
    virtual void send_cancel_all(uint64_t instrument_id) = 0;
    virtual void send_modify(const bpt::messages::ModifyOrder& modify, const std::string& native_symbol) = 0;

    [[nodiscard]] virtual bpt::messages::ExchangeId::Value exchange_id() const = 0;
    [[nodiscard]] virtual const char* exchange_name() const = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;

    // Drain all pending exec events from the adapter's IO thread into the
    // caller's thread.  Call this from the main poll loop on every iteration.
    // fn is invoked once per event; returns the number of events drained.
    virtual int drain_exec_events(const std::function<void(const ExecEvent&)>& fn) = 0;

    // Fetch current account positions and balance from the exchange REST API.
    // Blocking — must be called from a dedicated thread, not the poll loop.
    // Returns a populated AccountSnapshotData; throws std::exception on failure.
    virtual AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) = 0;
};

}  // namespace bpt::order_gateway::adapter
