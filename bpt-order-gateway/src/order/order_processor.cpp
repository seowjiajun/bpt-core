#include "order_gateway/order/order_processor.h"

#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/RejectReason.h>

#include <vector>
#include <yggdrasil/logging.h>
#include <yggdrasil/util/tsc_clock.h>

namespace {
inline uint64_t now_ns() noexcept {
    return ygg::util::TscClock::now_epoch_ns();
}
}  // namespace

namespace bpt::order_gateway::order {

OrderProcessor::OrderProcessor(messaging::ExecReportPublisher& exec_pub,
                               OrderStateManager& state_mgr,
                               risk::RiskChecker& risk_checker,
                               metrics::HeimdallMetrics& metrics,
                               const std::vector<std::shared_ptr<adapter::IOrderAdapter>>& adapters)
    : exec_pub_(exec_pub),
      state_mgr_(state_mgr),
      risk_checker_(risk_checker),
      metrics_(metrics) {
    adapter_by_id_.fill(nullptr);
    for (const auto& a : adapters) {
        auto idx = static_cast<uint8_t>(a->exchange_id());
        if (idx < adapter_by_id_.size())
            adapter_by_id_[idx] = a.get();
    }
    stale_ids_scratch_.reserve(64);
}

void OrderProcessor::on_exec_event(const adapter::ExecEvent& ev) {
    using OL = OrderLifecycle;

    const OL new_lc = exec_status_to_lifecycle(ev.status);

    // Use TscClock consistently for state_mgr timestamps so check_stale's
    // `cur_ns - last_update_ns` never underflows. Adapters set ev.local_ts_ns
    // from system_clock on their own thread, which can drift from TscClock
    // by a few ms and cause uint64 underflow in the stale check. The adapter
    // timestamp still flows through the published ExecReport below.
    state_mgr_.update(ev.order_id, new_lc, ev.exchange_order_id, ev.filled_qty, ev.remaining_qty, now_ns());

    // Release the open-order risk slot before publishing the exec report so
    // that Strategy can immediately place a replacement order if needed.
    const bool terminal = (new_lc == OL::FILLED || new_lc == OL::CANCELLED || new_lc == OL::REJECTED);
    if (terminal)
        risk_checker_.on_order_closed(ev.exchange_id);

    exec_pub_.publish(ev.order_id,
                      ev.exchange_order_id,
                      ev.exchange_id,
                      ev.instrument_id,
                      ev.status,
                      ev.side,
                      ev.order_type,
                      ev.price,
                      ev.filled_qty,
                      ev.remaining_qty,
                      ev.reject_reason,
                      ev.fee,
                      ev.fee_currency,
                      ev.exchange_ts_ns,
                      ev.local_ts_ns);

    const char* exch = exchange_str(ev.exchange_id);
    metrics_.exec_report(exch, lifecycle_str(new_lc)).Increment();

    // RTT = time from NewOrder insertion (created_ns) to first exchange ack.
    // Measures order placement latency end-to-end through Heimdall + exchange.
    if (new_lc == OL::ACKED) {
        if (const auto* st = state_mgr_.get(ev.order_id)) {
            if (ev.local_ts_ns > st->created_ns)
                metrics_.order_ack_rtt(exch).Observe(static_cast<double>(ev.local_ts_ns - st->created_ns));
        }
    }

    if (terminal)
        state_mgr_.remove(ev.order_id);
}

void OrderProcessor::on_new_order(const bpt::messages::NewOrder& order) {
    using ES = bpt::messages::ExecStatus;
    using RR = bpt::messages::RejectReason;
    using FC = bpt::messages::FeeCurrency;

    ygg::log::debug("[Heimdall] NewOrder: id={} exchange={} instrument_id={} qty={}",
                    order.orderId(),
                    static_cast<int>(order.exchangeId()),
                    order.instrumentId(),
                    order.quantity());

    const char* exch = exchange_str(order.exchangeId());
    metrics_.orders_received(exch).Increment();

    // Risk check must happen before state insertion so that a rejected order
    // never appears in the state manager.
    auto result =
        risk_checker_.check(order.exchangeId(), order.instrumentId(), order.price(), order.quantity(), order.orderId());
    if (!result) {
        const uint64_t ts = now_ns();
        exec_pub_.publish(order.orderId(),
                          0,
                          order.exchangeId(),
                          order.instrumentId(),
                          ES::REJECTED,
                          order.side(),
                          order.orderType(),
                          order.price(),
                          0,
                          order.quantity(),
                          result.error(),
                          0,
                          FC::USDT,
                          ts,
                          ts);
        ygg::log::warn("[Heimdall] Order {} rejected by risk: reason={}",
                       order.orderId(),
                       static_cast<int>(result.error()));
        metrics_.risk_reject(exch).Increment();
        return;
    }

    auto* adapter = find_adapter(order.exchangeId());
    if (!adapter || !adapter->is_connected()) {
        const uint64_t ts = now_ns();
        exec_pub_.publish(order.orderId(),
                          0,
                          order.exchangeId(),
                          order.instrumentId(),
                          ES::REJECTED,
                          order.side(),
                          order.orderType(),
                          order.price(),
                          0,
                          order.quantity(),
                          RR::EXCHANGE_ERROR,
                          0,
                          FC::USDT,
                          ts,
                          ts);
        ygg::log::warn("[Heimdall] Order {} rejected: adapter not connected", order.orderId());
        // Risk check already incremented the open-order counter — undo it so
        // the counter doesn't accumulate while the adapter is down.
        risk_checker_.on_order_closed(order.exchangeId());
        return;
    }

    // exchange_symbol is resolved by Strategy before publishing the NewOrder
    // message and carried in-band, so Heimdall never needs a symbol lookup.
    // We store it in state for cancel and modify operations.
    OrderState st;
    st.order_id = order.orderId();
    st.exchange_id = order.exchangeId();
    st.instrument_id = order.instrumentId();
    st.exchange_symbol = order.getExchangeSymbolAsString();
    st.side = order.side();
    st.order_type = order.orderType();
    st.price = order.price();
    st.quantity = order.quantity();
    st.remaining_qty = order.quantity();
    st.created_ns = now_ns();
    st.last_update_ns = st.created_ns;
    state_mgr_.insert(st);

    adapter->send_new_order(order);
}

void OrderProcessor::on_cancel(const bpt::messages::CancelOrder& cancel) {
    ygg::log::debug("[Heimdall] CancelOrder: id={} exchange={}",
                    cancel.orderId(),
                    static_cast<int>(cancel.exchangeId()));

    auto* adapter = find_adapter(cancel.exchangeId());
    if (!adapter)
        return;

    // exchange_symbol is needed by the adapter to build the exchange REST/WS
    // cancel request; it was stored at NewOrder time.
    const auto* st = state_mgr_.get(cancel.orderId());
    if (!st) {
        ygg::log::warn("[Heimdall] CancelOrder {}: order not found in state", cancel.orderId());
        return;
    }

    adapter->send_cancel(cancel, st->exchange_symbol);
}

void OrderProcessor::on_cancel_all(const bpt::messages::CancelAll& msg) {
    using EX = bpt::messages::ExchangeId;

    ygg::log::debug("[Heimdall] CancelAll: exchange={} instrument_id={}",
                    static_cast<int>(msg.exchangeId()),
                    msg.instrumentId());

    if (msg.exchangeId() == EX::ALL) {
        // Kill switch path — atomically disables trading so no new orders can
        // pass the risk check, then drains every open order across all venues.
        risk_checker_.set_trading_enabled(false);
        ygg::log::warn("[Heimdall] Kill switch activated — cancelling all open orders");
        state_mgr_.for_each_open([&](OrderState& st) {
            auto* adapter = find_adapter(st.exchange_id);
            if (adapter)
                adapter->send_cancel_all(st.instrument_id);
        });
    } else {
        auto* adapter = find_adapter(msg.exchangeId());
        if (adapter)
            adapter->send_cancel_all(msg.instrumentId());
    }
}

void OrderProcessor::on_modify(const bpt::messages::ModifyOrder& modify) {
    ygg::log::debug("[Heimdall] ModifyOrder: id={} exchange={}",
                    modify.orderId(),
                    static_cast<int>(modify.exchangeId()));

    auto* adapter = find_adapter(modify.exchangeId());
    if (!adapter)
        return;

    const auto* st = state_mgr_.get(modify.orderId());
    if (!st) {
        ygg::log::warn("[Heimdall] ModifyOrder {}: order not found in state", modify.orderId());
        return;
    }

    adapter->send_modify(modify, st->exchange_symbol);
}

void OrderProcessor::check_stale_orders(uint64_t stale_timeout_ns) {
    const uint64_t cur_ns = now_ns();

    // Collect stale IDs first, then remove — avoids mutating the map while
    // iterating over it in check_stale.
    stale_ids_scratch_.clear();
    state_mgr_.check_stale(cur_ns, stale_timeout_ns, [&](const OrderState& st) {
        ygg::log::warn("[Heimdall] Stale order detected: id={} exchange={} age_ms={}",
                       st.order_id,
                       static_cast<int>(st.exchange_id),
                       (cur_ns - st.last_update_ns) / 1'000'000ULL);

        metrics_.stale_orders_total->Increment();

        // Release the risk slot — the order is effectively dead regardless of
        // whether the exchange eventually sends a cancel confirmation.
        risk_checker_.on_order_closed(st.exchange_id);

        // Publish a synthetic CANCELLED exec report so Strategy can reconcile
        // its position and open-order book without waiting for the exchange.
        exec_pub_.publish(st.order_id,
                          st.exchange_order_id,
                          st.exchange_id,
                          st.instrument_id,
                          bpt::messages::ExecStatus::CANCELLED,
                          st.side,
                          st.order_type,
                          st.price,
                          st.filled_qty,
                          st.remaining_qty,
                          bpt::messages::RejectReason::OK,
                          0,
                          bpt::messages::FeeCurrency::USDT,
                          cur_ns,
                          cur_ns);

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
