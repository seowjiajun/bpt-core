#include "order_gateway/risk/pre_trade_risk_gate.h"

#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>

#include <bpt_common/logging.h>
#include <cmath>

namespace bpt::order_gateway::risk {

PreTradeRiskGate::PreTradeRiskGate(RiskChecker& risk_checker,
                                   PnlTracker& pnl_tracker,
                                   double max_position_usd,
                                   double max_daily_loss_usd,
                                   RejectRateBreaker::Config breaker_cfg)
    : risk_checker_(risk_checker),
      pnl_tracker_(pnl_tracker),
      max_position_usd_(max_position_usd),
      max_daily_loss_usd_(max_daily_loss_usd),
      reject_rate_breaker_(breaker_cfg) {}

std::optional<bpt::messages::RejectReason::Value> PreTradeRiskGate::evaluate(const order::NewOrderEvent& order) {
    using RR = bpt::messages::RejectReason;

    auto result =
        risk_checker_.check(order.exchange_id, order.instrument_id, order.price, order.quantity, order.order_id);
    if (!result)
        return result.error();

    if (max_position_usd_ > 0.0 && order.price > 0) {
        using bpt::messages::OrderSide;
        const int64_t cur_qty_e8 = pnl_tracker_.net_qty_e8(order.exchange_id, order.instrument_id);
        const int64_t delta_e8 = (order.side == OrderSide::BUY) ? static_cast<int64_t>(order.quantity)
                                                                : -static_cast<int64_t>(order.quantity);
        const double projected_qty = static_cast<double>(cur_qty_e8 + delta_e8) / 1e8;
        const double price = static_cast<double>(order.price) / 1e8;
        if (std::abs(projected_qty * price) > max_position_usd_) {
            risk_checker_.on_order_closed(order.exchange_id);
            return RR::RISK_REJECTED;
        }
    }

    return std::nullopt;
}

void PreTradeRiskGate::release_slot(bpt::messages::ExchangeId::Value exchange_id) {
    risk_checker_.on_order_closed(exchange_id);
}

void PreTradeRiskGate::on_exec_event(const adapter::ExecEvent& event) {
    using ES = bpt::messages::ExecStatus;

    const bool was_reject = (event.status == ES::REJECTED);
    const bool breaker_was_tripped = reject_rate_breaker_.tripped();
    reject_rate_breaker_.record(was_reject, event.local_ts_ns);
    if (!breaker_was_tripped && reject_rate_breaker_.tripped()) {
        risk_checker_.set_trading_enabled(false);
        bpt::common::log::error(
            "EXEC REJECT-RATE BREAKER TRIPPED — {}/{} exec events "
            "rejected in last {}s (threshold {:.1f}%). Trading halted. Restart "
            "service after human review to resume.",
            reject_rate_breaker_.rejects_in_window(),
            reject_rate_breaker_.total_in_window(),
            reject_rate_breaker_.config().window_ns / 1'000'000'000ULL,
            reject_rate_breaker_.config().threshold_pct);
    }

    if ((event.status == ES::FILLED || event.status == ES::PARTIAL) && event.filled_qty > 0) {
        pnl_tracker_.on_fill(event.exchange_id,
                             event.instrument_id,
                             event.side,
                             event.price,
                             event.filled_qty,
                             event.local_ts_ns);
        if (!daily_loss_latched_ && max_daily_loss_usd_ > 0.0) {
            const double daily = pnl_tracker_.daily_realized_pnl_usd(event.local_ts_ns);
            if (daily < -max_daily_loss_usd_) {
                daily_loss_latched_ = true;
                risk_checker_.set_trading_enabled(false);
                bpt::common::log::error(
                    "DAILY LOSS KILL SWITCH — realized P&L "
                    "{:.2f} USD < limit {:.2f} USD. Trading halted. "
                    "Restart service after human review to resume.",
                    daily,
                    -max_daily_loss_usd_);
            }
        }
    }

    const bool terminal = (event.status == ES::FILLED || event.status == ES::CANCELLED || event.status == ES::REJECTED);
    if (terminal)
        risk_checker_.on_order_closed(event.exchange_id);
}

void PreTradeRiskGate::disable_trading() noexcept {
    risk_checker_.set_trading_enabled(false);
}

bool PreTradeRiskGate::reject_rate_breaker_tripped() const noexcept {
    return reject_rate_breaker_.tripped();
}

}  // namespace bpt::order_gateway::risk
