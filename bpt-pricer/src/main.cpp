#include "pricer/app/pricer_app.h"
#include "pricer/config/settings.h"

#include <string>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>
#include <yggdrasil/signal.h>

int main(int argc, char** argv) {
    ygg::signal::install();

    std::string config_path = (argc > 1 && argv[1][0] != '-') ? argv[1] : "config/pricer.toml";
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--config")
            config_path = argv[i + 1];
    }

    bpt::pricer::config::Settings settings;
    try {
        settings = bpt::pricer::config::load(config_path);
    } catch (const std::exception& e) {
        ygg::logging::init("bpt-pricer");
        ygg::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    ygg::logging::LogConfig log_cfg;
    log_cfg.log_dir = settings.logging.dir;
    log_cfg.level = settings.logging.level;
    ygg::logging::init("bpt-pricer", log_cfg);
    ygg::log::info("Starting Pricer Volatility Surface Service...");

    ygg::log::info("[Pricer] publish_interval_ms={} risk_free_rate={:.4f}",
                   settings.publish_interval_ms,
                   settings.risk_free_rate);
    for (const auto& u : settings.underlyings)
        ygg::log::info("[Pricer] underlying: {}", u);
    for (const auto& e : settings.exchanges)
        ygg::log::info("[Pricer] exchange: {}", e);

    auto aeron = ygg::aeron::connect(settings.media_driver_dir);

    try {
        bpt::pricer::PricerApp app(std::move(settings), std::move(aeron));
        app.run();
    } catch (const std::exception& e) {
        ygg::log::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
