#include "strategy/app/strategy_app.h"
#include "strategy/config/config.h"

#include <CLI/CLI.hpp>
#include <memory>
#include <string>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-strategy — strategy engine"};
    std::string config_path = "config/fenrir.toml";
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::strategy::config::AppConfig app_cfg;
    try {
        app_cfg = bpt::strategy::config::AppConfig::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-strategy");
        bpt::common::log::error("{}", e.what());
        return 1;
    }

    try {
        return bpt::app::run("bpt-strategy", std::move(app_cfg),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                return std::make_unique<bpt::strategy::StrategyApp>(
                    std::move(cfg), ctx.aeron);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
