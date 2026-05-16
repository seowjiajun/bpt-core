// bridge — Aeron → WebSocket forwarder for the dashboard.

#include "bridge/app/bridge_service.h"
#include "bridge/config/settings.h"
#include "bridge/messaging/aeron_bus.h"
#include "bridge/messaging/publishers/i_dashboard_control_sink.h"
#include "bridge/ws/i_broadcaster.h"
#include "bridge/ws/ws_server.h"

#include <CLI/CLI.hpp>
#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/service_name.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    std::string strategy_override;
    std::string symbol_override;
    std::string exchange_override;
    std::string mode_override;
    std::string instrument_type_override;
    uint64_t instrument_id_override = 0;
    std::string profile_override;

    auto args =
        bpt::app::parse_cli(argc, argv, "bpt-bridge", "Aeron → WebSocket forwarder for dashboard", [&](CLI::App& cli) {
            cli.add_option("--strategy-name", strategy_override, "Override session.strategy");
            cli.add_option("--symbol", symbol_override, "Override session.symbol");
            cli.add_option("--exchange", exchange_override, "Override session.exchange");
            cli.add_option("--mode", mode_override, "Override session.mode (paper|live|mock)");
            cli.add_option("--instrument-type",
                           instrument_type_override,
                           "Override session.instrument_type (SPOT|PERP|FUTURE|OPTION)");
            cli.add_option("--instrument-id", instrument_id_override, "Override session.instrument_id");
            cli.add_option("--profile",
                           profile_override,
                           "Path to deployment profile TOML; overrides profile_config in the instance TOML. "
                           "Wired through the systemd env file so bridge labels its env from the active stack.");
        });

    // Initialise logging before config::load so the profile / aeron stream
    // map info lines surface in the journal. Re-inits with the role-qualified
    // service name once settings.exchange is known (same pattern as refdata
    // / md-gateway / order-gateway).
    bpt::common::logging::init("bpt-bridge");

    bpt::bridge::config::Settings settings;
    try {
        settings = bpt::bridge::config::load(args.config_path, profile_override);
    } catch (const std::exception& e) {
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    // CLI overrides take precedence over TOML values.
    if (!strategy_override.empty())
        settings.strategy = strategy_override;
    if (!symbol_override.empty())
        settings.symbol = symbol_override;
    if (!exchange_override.empty())
        settings.exchange = exchange_override;
    if (!mode_override.empty())
        settings.mode = mode_override;
    if (!instrument_type_override.empty())
        settings.instrument_type = instrument_type_override;
    if (instrument_id_override > 0)
        settings.instrument_id = instrument_id_override;

    const std::vector<std::string> venues =
        settings.exchange.empty() ? std::vector<std::string>{} : std::vector<std::string>{settings.exchange};
    const std::string service_name = bpt::common::util::derive_service_name("bridge", venues);
    bpt::common::logging::init(service_name);

    // WsServer lifecycle is owned here so its destructor (which calls stop())
    // runs after the service's unique_ptr has been released by bpt::app::run.
    // That ordering prevents the IO thread from delivering a command into a
    // half-destroyed BridgeService — BridgeService::run() also detaches the
    // command handler on shutdown, which is the primary guard.
    auto ws_server = std::make_shared<bpt::bridge::WsServer>(settings.ws_port);
    ws_server->start();

    try {
        const int rc = bpt::app::run(
            service_name,
            std::move(settings),
            [ws_server](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                // Composition root: build the bus, surface the control publisher
                // as the IDashboardControlSink port, hand the WsServer as the
                // IBroadcaster port.
                auto bus = bpt::bridge::messaging::BridgeAeronBus::build(ctx.aeron, cfg);
                std::shared_ptr<bpt::bridge::messaging::IDashboardControlSink> ctrl_sink = bus.ctrl_pub;
                return std::make_unique<bpt::bridge::BridgeService>(std::move(cfg),
                                                                    std::move(bus),
                                                                    ws_server,
                                                                    std::move(ctrl_sink));
            });
        ws_server->stop();
        return rc;
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        ws_server->stop();
        return 1;
    }
}
