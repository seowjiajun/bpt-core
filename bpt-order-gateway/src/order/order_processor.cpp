#include "order_gateway/order/order_processor.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/RejectReason.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <vector>

using bpt::order_gateway::order::CancelAllEvent;
using bpt::order_gateway::order::CancelOrderEvent;
using bpt::order_gateway::order::ModifyOrderEvent;
using bpt::order_gateway::order::NewOrderEvent;

namespace {
inline uint64_t now_ns() noexcept {
    return bpt::common::util::TscClock::now_epoch_ns();
}
}  // namespace

namespace bpt::order_gateway::order {

OrderProcessor::OrderProcessor(messaging::api::ExecReportPublisher& exec_pub,
                               OrderStateManager& state_mgr,
                               risk::PreTradeRiskGate& risk_gate,
                               metrics::OrderGatewayMetrics& metrics,
                               const std::vector<std::shared_ptr<adapter::IOrderAdapter>>& adapters)
    : exec_pub_(exec_pub),
      state_mgr_(state_mgr),
      risk_gate_(risk_gate),
      metrics_(metrics) {
    adapter_by_id_.fill(nullptr);
    for (const auto& a : adapters) {
        auto idx = static_cast<uint8_t>(a->exchange_id());
        if (idx < adapter_by_id_.size())
            adapter_by_id_[idx] = a.get();
    }
    stale_ids_scratch_.reserve(64);
}

void OrderProcessor::on_exec_event(const adapter::ExecEvent& event) {
    using OL = OrderLifecycle;

    const OL new_lc = exec_status_to_lifecycle(event.status);

    // Use TscClock consistently for state_mgr timestamps so check_stale's
    // `cur_ns - last_update_ns` never underflows. Adapters set event.local_ts_ns
    // from system_clock on their own thread, which can drift from TscClock
    // by a few ms and cause uint64 underflow in the stale check. The adapter
    // timestamp still flows through the published ExecReport below.
    state_mgr_.update(event.order_id, new_lc, event.exchange_order_id, event.filled_qty, event.remaining_qty, now_ns());

    risk_gate_.on_exec_event(event);

    exec_pub_.publish(event.to_report());

    const char* exch = exchange_str(event.exchange_id);
    metrics_.exec_report(exch, lifecycle_str(new_lc)).Increment();

    // RTT = time from NewOrder insertion (created_ns) to first exchange ack.
    if (new_lc == OL::ACKED) {
        if (const auto* st = state_mgr_.get(event.order_id)) {
            if (event.local_ts_ns > st->created_ns)
                metrics_.order_ack_rtt(exch).Observe(static_cast<double>(event.local_ts_ns - st->created_ns));
        }
    }

    const bool terminal = (new_lc == OL::FILLED || new_lc == OL::CANCELLED || new_lc == OL::REJECTED);
    if (terminal)
        state_mgr_.remove(event.order_id);
}

void OrderProcessor::on_new_order(const NewOrderEvent& order) {
    using RR = bpt::messages::RejectReason;

    bpt::common::log::debug("NewOrder: id={} exchange={} instrument_id={} qty={}",
                            order.order_id,
                            static_cast<int>(order.exchange_id),
                            order.instrument_id,
                            order.quantity);

    const char* exch = exchange_str(order.exchange_id);
    metrics_.orders_received(exch).Increment();

    if (const auto reason = risk_gate_.evaluate(order)) {
        exec_pub_.publish(order.to_reject_report(*reason, now_ns()));
        bpt::common::log::warn("Order {} rejected by risk: reason={}", order.order_id, static_cast<int>(*reason));
        metrics_.risk_reject(exch).Increment();
        return;
    }

    // Adapter availability check — routing concern, not risk policy.
    // Two halt paths:
    //   - !is_connected()  transient (reconnecting) or permanently gone
    //   - is_halted()      disconnect-rate breaker latched, operator restart required
    auto* adapter = find_adapter(order.exchange_id);
    if (!adapter || !adapter->is_connected() || adapter->is_halted()) {
        exec_pub_.publish(order.to_reject_report(RR::EXCHANGE_ERROR, now_ns()));
        if (adapter && adapter->is_halted())
            bpt::common::log::warn("Order {} rejected: {} halted by disconnect breaker",
                                   order.order_id,
                                   adapter->exchange_name());
        else
            bpt::common::log::warn("Order {} rejected: adapter not connected", order.order_id);
        risk_gate_.release_slot(order.exchange_id);
        return;
    }

    state_mgr_.insert(order.to_order_state(now_ns()));

    adapter->send_new_order(order);
}

void OrderProcessor::on_cancel(const CancelOrderEvent& cancel) {
    bpt::common::log::debug("CancelOrder: id={} exchange={}", cancel.order_id, static_cast<int>(cancel.exchange_id));

    auto* adapter = find_adapter(cancel.exchange_id);
    if (!adapter)
        return;

    const auto* st = state_mgr_.get(cancel.order_id);
    if (!st) {
        bpt::common::log::warn("CancelOrder {}: order not found in state", cancel.order_id);
        return;
    }

    adapter->send_cancel(cancel, st->exchange_symbol);
}

void OrderProcessor::on_cancel_all(const CancelAllEvent& cancel_all) {
    using EX = bpt::messages::ExchangeId;

    bpt::common::log::debug("CancelAll: exchange={} instrument_id={}",
                            static_cast<int>(cancel_all.exchange_id),
                            cancel_all.instrument_id);

    if (cancel_all.exchange_id == EX::ALL) {
        // Kill switch path — atomically disables trading, then drains every
        // open order across all venues.
        risk_gate_.disable_trading();
        bpt::common::log::warn("Kill switch activated — cancelling all open orders");
        state_mgr_.for_each_open([&](OrderState& st) {
            auto* adapter = find_adapter(st.exchange_id);
            if (adapter)
                adapter->send_cancel_all(st.instrument_id);
        });
    } else {
        auto* adapter = find_adapter(cancel_all.exchange_id);
        if (adapter)
            adapter->send_cancel_all(cancel_all.instrument_id);
    }
}

void OrderProcessor::on_modify(const ModifyOrderEvent& modify) {
    bpt::common::log::debug("ModifyOrder: id={} exchange={}", modify.order_id, static_cast<int>(modify.exchange_id));

    auto* adapter = find_adapter(modify.exchange_id);
    if (!adapter)
        return;

    const auto* st = state_mgr_.get(modify.order_id);
    if (!st) {
        bpt::common::log::warn("ModifyOrder {}: order not found in state", modify.order_id);
        return;
    }

    adapter->send_modify(modify, st->exchange_symbol);
}

void OrderProcessor::check_stale_orders(uint64_t stale_timeout_ns) {
    const uint64_t cur_ns = now_ns();

    stale_ids_scratch_.clear();
    state_mgr_.check_stale(cur_ns, stale_timeout_ns, [&](const OrderState& st) {
        bpt::common::log::warn("Stale order detected: id={} exchange={} age_ms={}",
                               st.order_id,
                               static_cast<int>(st.exchange_id),
                               (cur_ns - st.last_update_ns) / 1'000'000ULL);

        metrics_.stale_orders_total->Increment();
        risk_gate_.release_slot(st.exchange_id);
        exec_pub_.publish(st.to_cancel_report(cur_ns));
        stale_ids_scratch_.push_back(st.order_id);
    });

    for (uint64_t id : stale_ids_scratch_)
        state_mgr_.remove(id);
}

// ----- private helpers -------------------------------------------------------

adapter::IOrderAdapter* OrderProcessor::find_adapter(bpt::messages::ExchangeId::Value id) const {
    auto idx = static_cast<uint8_t>(id);
    if (idx >= adapter_by_id_.size())
        return nullptr;
    return adapter_by_id_[idx];
}

OrderLifecycle OrderProcessor::exec_status_to_lifecycle(bpt::messages::ExecStatus::Value status) {
    using ES = bpt::messages::ExecStatus;
    using OL = OrderLifecycle;
    switch (status) {
        case ES::ACKED:
            return OL::ACKED;
        case ES::PARTIAL:
            return OL::PARTIALLY_FILLED;
        case ES::FILLED:
            return OL::FILLED;
        case ES::CANCELLED:
            return OL::CANCELLED;
        case ES::REJECTED:
            return OL::REJECTED;
        default:
            return OL::ACKED;
    }
}

const char* OrderProcessor::lifecycle_str(OrderLifecycle lc) {
    using OL = OrderLifecycle;
    switch (lc) {
        case OL::ACKED:
            return "ACKED";
        case OL::PARTIALLY_FILLED:
            return "PARTIAL";
        case OL::FILLED:
            return "FILLED";
        case OL::CANCELLED:
            return "CANCELLED";
        case OL::REJECTED:
            return "REJECTED";
        default:
            return "UNKNOWN";
    }
}

const char* OrderProcessor::exchange_str(bpt::messages::ExchangeId::Value id) {
    using EX = bpt::messages::ExchangeId;
    switch (id) {
        case EX::BINANCE:
            return "BINANCE";
        case EX::OKX:
            return "OKX";
        case EX::HYPERLIQUID:
            return "HYPERLIQUID";
        case EX::DERIBIT:
            return "DERIBIT";
        default:
            return "UNKNOWN";
    }
}

}  // namespace bpt::order_gateway::order
