#include "pricer/app/pricer_app.h"
#include "pricer/config/settings.h"

#include <CLI/CLI.hpp>
#include <string>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>

int main(int argc, char** argv) {
    bpt::common::signal::install();

    CLI::App app{"bpt-pricer — options Greeks + vol surface pricer"};
    std::string config_path = "config/pricer.toml";
    app.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(app, argc, argv);

    bpt::pricer::config::Settings settings;
    try {
        settings = bpt::pricer::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-pricer");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    bpt::common::logging::LogConfig log_cfg;
    log_cfg.log_dir = settings.logging.dir;
    log_cfg.level = settings.logging.level;
    bpt::common::logging::init("bpt-pricer", log_cfg);
    bpt::common::log::info("Starting Pricer Volatility Surface Service...");

    bpt::common::log::info("[Pricer] publish_interval_ms={} risk_free_rate={:.4f}",
                   settings.publish_interval_ms,
                   settings.risk_free_rate);
    for (const auto& u : settings.underlyings)
        bpt::common::log::info("[Pricer] underlying: {}", u);
    for (const auto& e : settings.exchanges)
        bpt::common::log::info("[Pricer] exchange: {}", e);

    auto aeron = bpt::common::aeron::connect(settings.media_driver_dir);

    try {
        bpt::pricer::PricerApp app(std::move(settings), std::move(aeron));
        app.run();
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
