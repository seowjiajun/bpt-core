#pragma once

#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/order/inbound_order_events.h"
#include "order_gateway/risk/pnl_tracker.h"
#include "order_gateway/risk/reject_rate_breaker.h"
#include "order_gateway/risk/risk_checker.h"

#include <messages/ExchangeId.h>
#include <messages/RejectReason.h>

#include <optional>

namespace bpt::order_gateway::risk {

/// \brief Pre-trade risk gate and post-trade risk monitor for OrderProcessor.
///
/// Owns all risk state that does not belong in OrderProcessor's routing logic:
///   - Pre-trade: size/notional/open-order/rate-limit check (via RiskChecker)
///     and position-cap check (via PnlTracker).
///   - Post-trade: reject-rate circuit breaker, daily-loss kill switch,
///     and open-order slot release on terminal exec events.
///
/// Single-writer: all methods must be called from the main poll thread.
class PreTradeRiskGate {
public:
    PreTradeRiskGate(RiskChecker& risk_checker,
                     PnlTracker& pnl_tracker,
                     double max_position_usd,
                     double max_daily_loss_usd,
                     RejectRateBreaker::Config breaker_cfg);

    /// \brief Pre-trade check. Returns reject reason on fail, nullopt on pass.
    ///
    /// On pass, an open-order risk slot is claimed inside RiskChecker::check().
    /// The caller must either route the order (slot released on terminal exec
    /// event via on_exec_event()) or call release_slot() if routing fails.
    [[nodiscard]] std::optional<bpt::messages::RejectReason::Value> evaluate(const order::NewOrderEvent& order);

    /// \brief Release a slot claimed by a passing evaluate() when the order
    /// cannot be routed to an adapter (adapter down or halted).
    void release_slot(bpt::messages::ExchangeId::Value exchange_id);

    /// \brief Must be called for every exchange exec event.
    ///
    /// Updates the reject-rate breaker, daily-loss kill switch, and releases
    /// the open-order risk slot for terminal outcomes (FILLED, CANCELLED, REJECTED).
    void on_exec_event(const adapter::ExecEvent& event);

    void disable_trading() noexcept;
    [[nodiscard]] bool reject_rate_breaker_tripped() const noexcept;

private:
    RiskChecker& risk_checker_;
    PnlTracker& pnl_tracker_;
    double max_position_usd_;
    double max_daily_loss_usd_;
    bool daily_loss_latched_{false};
    RejectRateBreaker reject_rate_breaker_;
};

}  // namespace bpt::order_gateway::risk
