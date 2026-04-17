#include "analytics/app/tyr_app.h"
#include "analytics/config/settings.h"

#include <string>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>
#include <yggdrasil/signal.h>

int main(int argc, char** argv) {
    ygg::signal::install();

    std::string config_path = (argc > 1 && argv[1][0] != '-') ? argv[1] : "config/tyr.toml";
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--config")
            config_path = argv[i + 1];
    }

    bpt::analytics::config::Settings settings;
    try {
        settings = bpt::analytics::config::load(config_path);
    } catch (const std::exception& e) {
        ygg::logging::init("bpt-analytics");
        ygg::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    ygg::logging::LogConfig log_cfg;
    log_cfg.log_dir = settings.logging.dir;
    log_cfg.level = settings.logging.level;
    ygg::logging::init("bpt-analytics", log_cfg);
    ygg::log::info("Starting Analytics Toxic Flow Analyzer...");

    auto aeron = ygg::aeron::connect(settings.media_driver_dir);

    try {
        bpt::analytics::AnalyticsApp app(std::move(settings), std::move(aeron));
        app.run();
    } catch (const std::exception& e) {
        ygg::log::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
