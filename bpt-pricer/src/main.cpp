#include "pricer/app/pricer_app.h"
#include "pricer/config/settings.h"
#include "pricer/messaging/aeron_bus.h"

#include <CLI/CLI.hpp>
#include <memory>
#include <string>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>

int main(int argc, char** argv) {
    CLI::App cli{"bpt-pricer — options Greeks + vol surface pricer"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::pricer::config::Settings settings;
    try {
        settings = bpt::pricer::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-pricer");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    try {
        return bpt::app::run("bpt-pricer", std::move(settings),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                auto bus = bpt::pricer::messaging::PricerAeronBus::build(ctx.aeron, cfg);
                return std::make_unique<bpt::pricer::PricerApp>(
                    std::move(cfg), std::move(bus));
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
