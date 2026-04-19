#include "analytics/app/tyr_app.h"
#include "analytics/config/settings.h"

#include <CLI/CLI.hpp>
#include <string>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>

int main(int argc, char** argv) {
    bpt::common::signal::install();

    CLI::App app{"bpt-analytics — markouts, toxicity, fill-rate analytics"};
    std::string config_path = "config/tyr.toml";
    app.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(app, argc, argv);

    bpt::analytics::config::Settings settings;
    try {
        settings = bpt::analytics::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-analytics");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    bpt::common::logging::LogConfig log_cfg;
    log_cfg.log_dir = settings.logging.dir;
    log_cfg.level = settings.logging.level;
    bpt::common::logging::init("bpt-analytics", log_cfg);
    bpt::common::log::info("Starting Analytics Toxic Flow Analyzer...");

    auto aeron = bpt::common::aeron::connect(settings.media_driver_dir);

    try {
        bpt::analytics::AnalyticsApp app(std::move(settings), std::move(aeron));
        app.run();
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
