#pragma once

/// @file
/// BridgeService — Aeron → WebSocket forwarder for the dashboard.
///
/// Wraps `bpt::app::IService` so main() follows the same lifecycle pattern
/// as every other bpt-core service. Constructor takes a pre-built
/// `BridgeBus` (built by BridgeAeronBus::build at the prod composition
/// root) plus the two output ports — IBroadcaster (dashboard WS sink) and
/// api::DashboardControlPublisher (bridge → strategy command sink). No
/// `<Aeron.h>` in this header.
///
/// The event-handler methods (on_*) are public so tests can drive them
/// directly without polling the bus, and can substitute Fake implementations
/// of the two output ports. See bpt-bridge/tests/unit/test_bridge_service_seam.cpp.

#include "bridge/config/settings.h"
#include "bridge/messaging/aeron_bus.h"
#include "bridge/messaging/publishers/api/dashboard_control_publisher.h"
#include "bridge/messaging/subscribers/api/account_subscriber.h"
#include "bridge/messaging/subscribers/api/exec_subscriber.h"
#include "bridge/state/position_tracker.h"
#include "bridge/ws/i_broadcaster.h"

#include <analytics/messaging/toxicity_update.h>
#include <radar/messaging/market_color.h>

#include <bpt_app/app.h>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace bpt::bridge {

class BridgeService : public bpt::app::IService {
public:
    BridgeService(config::Settings settings,
                  messaging::BridgeBus bus,
                  std::shared_ptr<ws::IBroadcaster> broadcaster,
                  std::shared_ptr<messaging::api::DashboardControlPublisher> ctrl_sink);

    void run() override;

    // ── Event handlers ────────────────────────────────────────────────────────
    // Public so tests can drive them directly, bypassing the Aeron poll loop.
    // In production, run() wires bus_.<...>->set_handler(...) to call these.

    /// MD market-data tick. Filters by settings_.instrument_id if set, throttles
    /// dashboard broadcasts to ~30 Hz, and updates last_mid_ for unrealized PnL.
    void on_md_tick(uint64_t instrument_id, double mid, uint64_t ts_ns);

    /// OrderGateway exec-report fill (FILLED or PARTIAL with qty > 0). Updates
    /// position tracker, broadcasts Fill + Position messages.
    void on_exec_fill(const messaging::api::ExecSubscriber::Fill& f);

    /// OrderGateway exec-report lifecycle event (all statuses). Broadcasts
    /// an Order message so the dashboard can track working/cancelled orders.
    void on_exec_order_event(const messaging::api::ExecSubscriber::OrderEvent& ev);

    /// OrderGateway AccountSnapshot — equity / balance / per-currency rows.
    void on_account_snapshot(const messaging::api::AccountSubscriber::Snapshot& s);

    /// Strategy portfolio-snapshot JSON, already reassembled if it spanned
    /// multiple Aeron fragments.
    void on_portfolio_json(std::string_view json);

    /// Analytics toxicity update.
    void on_toxicity(const bpt::analytics::messaging::ToxicityUpdate& u);

    /// Radar market-color update.
    void on_market_color(const bpt::radar::messaging::MarketColor& mc);

    /// Dashboard command from the WS client — currently "halt" or "resume".
    /// Publishes the control byte via ctrl_sink_ and broadcasts a status msg.
    void on_dashboard_command(const std::string& cmd);

private:
    void publish_session_init();

    config::Settings settings_;
    messaging::BridgeBus bus_;
    std::shared_ptr<ws::IBroadcaster> broadcaster_;
    std::shared_ptr<messaging::api::DashboardControlPublisher> ctrl_sink_;

    PositionTracker tracker_;
    double last_mid_ = 0.0;

    // Tick throttle — BBO mids update ~1000 Hz but the dashboard only
    // needs ~30 Hz for a smooth visual. Most-recent value wins.
    std::chrono::steady_clock::time_point last_tick_bcast_{};
    static constexpr std::chrono::milliseconds kTickMinInterval{33};
};

}  // namespace bpt::bridge
