#include "surtr/app/surtr_app.h"
#include "surtr/config/settings.h"

#include <string>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>
#include <yggdrasil/signal.h>

int main(int argc, char** argv) {
    ygg::signal::install();

    std::string config_path = (argc > 1 && argv[1][0] != '-') ? argv[1] : "config/surtr.toml";
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--config")
            config_path = argv[i + 1];
    }

    surtr::config::Settings settings;
    try {
        settings = surtr::config::load(config_path);
    } catch (const std::exception& e) {
        ygg::logging::init("surtr");
        ygg::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    ygg::logging::LogConfig log_cfg;
    log_cfg.log_dir = settings.logging.dir;
    log_cfg.level = settings.logging.level;
    ygg::logging::init("surtr", log_cfg);
    ygg::log::info("Starting Surtr Volatility Surface Service...");

    ygg::log::info("[Surtr] publish_interval_ms={} risk_free_rate={:.4f}",
                   settings.publish_interval_ms,
                   settings.risk_free_rate);
    for (const auto& u : settings.underlyings)
        ygg::log::info("[Surtr] underlying: {}", u);
    for (const auto& e : settings.exchanges)
        ygg::log::info("[Surtr] exchange: {}", e);

    auto aeron = ygg::aeron::connect(settings.media_driver_dir);

    try {
        surtr::SurtrApp app(std::move(settings), std::move(aeron));
        app.run();
    } catch (const std::exception& e) {
        ygg::log::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
