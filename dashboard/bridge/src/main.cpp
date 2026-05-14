// bridge — Aeron → WebSocket forwarder for the dashboard.

#include "bridge/app/bridge_service.h"
#include "bridge/config/settings.h"

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

    bridge::config::Settings settings;
    try {
        settings = bridge::config::load(args.config_path, profile_override);
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

    try {
        return bpt::app::run(service_name,
                             std::move(settings),
                             [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                                 return std::make_unique<bridge::BridgeService>(std::move(cfg), ctx.aeron);
                             });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
