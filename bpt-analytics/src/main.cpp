#include "analytics/app/tyr_app.h"
#include "analytics/config/settings.h"
#include "analytics/messaging/aeron_bus.h"

#include <CLI/CLI.hpp>
#include <memory>
#include <string>
#include <bpt_app/app.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>

int main(int argc, char** argv) {
    CLI::App cli{"bpt-analytics — markouts, toxicity, fill-rate analytics"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::analytics::config::Settings settings;
    try {
        settings = bpt::analytics::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-analytics");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    // Optional fault injection (dev/qa only). Must run before bpt::app::run
    // builds the AeronBus — Subscribers consult the registry at ctor time.
    try {
        bpt::common::aeron::install_chaos_from_toml(
            config_path,
            bpt::common::to_string(settings.base.environment),
            "bpt-analytics");
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-analytics");
        bpt::common::log::error("[chaos] config rejected: {}", e.what());
        return 1;
    }

    try {
        return bpt::app::run("bpt-analytics", std::move(settings),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                auto bus = bpt::analytics::messaging::AnalyticsAeronBus::build(ctx.aeron, cfg);
                return std::make_unique<bpt::analytics::AnalyticsApp>(
                    std::move(cfg), std::move(bus));
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
