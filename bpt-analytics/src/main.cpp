#include "analytics/app/tyr_app.h"
#include "analytics/config/settings.h"

#include <CLI/CLI.hpp>
#include <memory>
#include <string>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>

int main(int argc, char** argv) {
    CLI::App cli{"bpt-analytics — markouts, toxicity, fill-rate analytics"};
    std::string config_path = "config/tyr.toml";
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
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

    try {
        return bpt::app::run("bpt-analytics", std::move(settings),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                return std::make_unique<bpt::analytics::AnalyticsApp>(
                    std::move(cfg), ctx.aeron);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
