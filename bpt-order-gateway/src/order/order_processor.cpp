#include "order_gateway/order/order_processor.h"

#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/OrderSide.h>
#include <messages/RejectReason.h>

#include <cmath>
#include <vector>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace {
inline uint64_t now_ns() noexcept {
    return bpt::common::util::TscClock::now_epoch_ns();
}
}  // namespace

namespace bpt::order_gateway::order {

OrderProcessor::OrderProcessor(messaging::IExecReportPublisher& exec_pub,
                               OrderStateManager& state_mgr,
                               risk::RiskChecker& risk_checker,
                               risk::PnlTracker& pnl_tracker,
                               double max_daily_loss_usd,
                               double max_position_usd,
                               risk::RejectRateBreaker::Config breaker_cfg,
                               metrics::OrderGatewayMetrics& metrics,
                               const std::vector<std::shared_ptr<adapter::IOrderAdapter>>& adapters)
    : exec_pub_(exec_pub),
      state_mgr_(state_mgr),
      risk_checker_(risk_checker),
      pnl_tracker_(pnl_tracker),
      max_daily_loss_usd_(max_daily_loss_usd),
      max_position_usd_(max_position_usd),
      reject_rate_breaker_(breaker_cfg),
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
    using ES = bpt::messages::ExecStatus;

    const OL new_lc = exec_status_to_lifecycle(ev.status);

    // Use TscClock consistently for state_mgr timestamps so check_stale's
    // `cur_ns - last_update_ns` never underflows. Adapters set ev.local_ts_ns
    // from system_clock on their own thread, which can drift from TscClock
    // by a few ms and cause uint64 underflow in the stale check. The adapter
    // timestamp still flows through the published ExecReport below.
    state_mgr_.update(ev.order_id, new_lc, ev.exchange_order_id, ev.filled_qty, ev.remaining_qty, now_ns());

    // Exchange-reject-rate circuit breaker. Feeds every exec event into
    // the rolling-window tracker; on trip we flip the same RiskChecker
    // latch the daily-loss kill switch uses. Intended signal: the
    // exchange is refusing our orders en masse (bad creds, wrong account
    // mode, geo-block, margin-mode flip) — we want to stop hammering
    // before the operator notices.
    const bool was_reject = (ev.status == ES::REJECTED);
    const bool breaker_was_tripped = reject_rate_breaker_.tripped();
    reject_rate_breaker_.record(was_reject, ev.local_ts_ns);
    if (!breaker_was_tripped && reject_rate_breaker_.tripped()) {
        risk_checker_.set_trading_enabled(false);
        bpt::common::log::error(
            "[OrderGateway] EXEC REJECT-RATE BREAKER TRIPPED — {}/{} exec events "
            "rejected in last {}s (threshold {:.1f}%). Trading halted. Restart "
            "service after human review to resume.",
            reject_rate_breaker_.rejects_in_window(),
            reject_rate_breaker_.total_in_window(),
            reject_rate_breaker_.config().window_ns / 1'000'000'000ULL,
            reject_rate_breaker_.config().threshold_pct);
    }

    // Daily-loss kill switch. Every FILLED/PARTIAL fill updates realized
    // P&L; if it crosses below -max_daily_loss_usd we flip the existing
    // RiskChecker kill switch so subsequent NewOrders reject at the
    // pretrade gate. The latch intentionally sticks across UTC midnight
    // rollovers — re-enabling requires a restart so an operator has a
    // chance to look at WHY we lost that much today.
    if ((ev.status == ES::FILLED || ev.status == ES::PARTIAL) && ev.filled_qty > 0) {
        pnl_tracker_.on_fill(ev.exchange_id, ev.instrument_id, ev.side,
                             ev.price, ev.filled_qty, ev.local_ts_ns);
        if (!daily_loss_latched_ && max_daily_loss_usd_ > 0.0) {
            const double daily = pnl_tracker_.daily_realized_pnl_usd(ev.local_ts_ns);
            if (daily < -max_daily_loss_usd_) {
                daily_loss_latched_ = true;
                risk_checker_.set_trading_enabled(false);
                bpt::common::log::error(
                    "[OrderGateway] DAILY LOSS KILL SWITCH — realized P&L "
                    "{:.2f} USD < limit {:.2f} USD. Trading halted. "
                    "Restart service after human review to resume.",
                    daily, -max_daily_loss_usd_);
            }
        }
    }

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
    // Measures order placement latency end-to-end through OrderGateway + exchange.
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

    bpt::common::log::debug("[OrderGateway] NewOrder: id={} exchange={} instrument_id={} qty={}",
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
        bpt::common::log::warn("[OrderGateway] Order {} rejected by risk: reason={}",
                       order.orderId(),
                       static_cast<int>(result.error()));
        metrics_.risk_reject(exch).Increment();
        return;
    }

    // Position-cap check: projected net position × order price vs limit.
    // Uses the order's own price as the mark — exact at fill time, and
    // the only price the order-gateway has access to (no MD
    // subscription here). Skipped for MARKET orders (price == 0) since
    // the projection has no meaningful mark; rely on max_order_size_usd
    // to cap those.
    if (max_position_usd_ > 0.0 && order.price() > 0) {
        using bpt::messages::OrderSide;
        const int64_t cur_qty_e8 =
            pnl_tracker_.net_qty_e8(order.exchangeId(), order.instrumentId());
        const int64_t delta_e8 = (order.side() == OrderSide::BUY)
            ? static_cast<int64_t>(order.quantity())
            : -static_cast<int64_t>(order.quantity());
        const int64_t projected_qty_e8 = cur_qty_e8 + delta_e8;
        const double projected_qty = static_cast<double>(projected_qty_e8) / 1e8;
        const double price = static_cast<double>(order.price()) / 1e8;
        const double projected_usd = std::abs(projected_qty * price);
        if (projected_usd > max_position_usd_) {
            const uint64_t ts = now_ns();
            exec_pub_.publish(order.orderId(), 0, order.exchangeId(), order.instrumentId(),
                              ES::REJECTED, order.side(), order.orderType(), order.price(),
                              0, order.quantity(), RR::RISK_REJECTED, 0, FC::USDT, ts, ts);
            bpt::common::log::warn("[OrderGateway] Order {} rejected by position cap: "
                           "projected=${:.2f} > limit=${:.2f}",
                           order.orderId(), projected_usd, max_position_usd_);
            metrics_.risk_reject(exch).Increment();
            // Release the risk-checker slot since we've rejected — it was
            // incremented inside risk_checker_.check above.
            risk_checker_.on_order_closed(order.exchangeId());
            return;
        }
    }

    auto* adapter = find_adapter(order.exchangeId());
    // Two adapter-level halt paths:
    //   - !is_connected()   transient (reconnecting) or permanently gone
    //   - is_halted()       disconnect-rate breaker latched, operator restart
    //                       required. Distinct log so ops can tell them apart.
    if (!adapter || !adapter->is_connected() || adapter->is_halted()) {
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
        if (adapter && adapter->is_halted()) {
            bpt::common::log::warn("[OrderGateway] Order {} rejected: {} halted by disconnect breaker",
                           order.orderId(), adapter->exchange_name());
        } else {
            bpt::common::log::warn("[OrderGateway] Order {} rejected: adapter not connected", order.orderId());
        }
        // Risk check already incremented the open-order counter — undo it so
        // the counter doesn't accumulate while the adapter is down.
        risk_checker_.on_order_closed(order.exchangeId());
        return;
    }

    // exchange_symbol is resolved by Strategy before publishing the NewOrder
    // message and carried in-band, so OrderGateway never needs a symbol lookup.
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
    bpt::common::log::debug("[OrderGateway] CancelOrder: id={} exchange={}",
                    cancel.orderId(),
                    static_cast<int>(cancel.exchangeId()));

    auto* adapter = find_adapter(cancel.exchangeId());
    if (!adapter)
        return;

    // exchange_symbol is needed by the adapter to build the exchange REST/WS
    // cancel request; it was stored at NewOrder time.
    const auto* st = state_mgr_.get(cancel.orderId());
    if (!st) {
        bpt::common::log::warn("[OrderGateway] CancelOrder {}: order not found in state", cancel.orderId());
        return;
    }

    adapter->send_cancel(cancel, st->exchange_symbol);
}

void OrderProcessor::on_cancel_all(const bpt::messages::CancelAll& msg) {
    using EX = bpt::messages::ExchangeId;

    bpt::common::log::debug("[OrderGateway] CancelAll: exchange={} instrument_id={}",
                    static_cast<int>(msg.exchangeId()),
                    msg.instrumentId());

    if (msg.exchangeId() == EX::ALL) {
        // Kill switch path — atomically disables trading so no new orders can
        // pass the risk check, then drains every open order across all venues.
        risk_checker_.set_trading_enabled(false);
        bpt::common::log::warn("[OrderGateway] Kill switch activated — cancelling all open orders");
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
    bpt::common::log::debug("[OrderGateway] ModifyOrder: id={} exchange={}",
                    modify.orderId(),
                    static_cast<int>(modify.exchangeId()));

    auto* adapter = find_adapter(modify.exchangeId());
    if (!adapter)
        return;

    const auto* st = state_mgr_.get(modify.orderId());
    if (!st) {
        bpt::common::log::warn("[OrderGateway] ModifyOrder {}: order not found in state", modify.orderId());
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
        bpt::common::log::warn("[OrderGateway] Stale order detected: id={} exchange={} age_ms={}",
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
