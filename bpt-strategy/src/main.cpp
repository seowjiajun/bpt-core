#include "strategy/app/strategy_app.h"
#include "strategy/config/config.h"

#include <CLI/CLI.hpp>
#include <memory>
#include <string>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>

namespace {

// Role-qualified service name: "bpt-strat-<variant>" where variant is
// a short code for cfg.strat.strategy.type. Hosts routinely run the
// same strategy binary multiple times (canary alongside live, A/B
// variants, different instrument universes), so the variant code
// makes `top -H` and log files instance-aware.
// Length budget: "bpt-strat-" is 10 chars → up to 5 chars for variant
// fits inside the 15-char comm cap; unknown types fall back to the
// first 5 chars of the type string.
std::string derive_service_name(const std::string& strategy_type) {
    std::string variant;
    if (strategy_type == "avellaneda_stoikov") variant = "as";
    else if (strategy_type == "regime_switch")  variant = "rs";
    else if (strategy_type == "ofi")            variant = "ofi";
    else if (strategy_type == "hmm")            variant = "hmm";
    else if (strategy_type == "momentum")       variant = "mom";
    else                                         variant = strategy_type.substr(0, 5);
    return "bpt-strat-" + variant;
}

}  // namespace

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-strategy — strategy engine"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
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

    const std::string service_name = derive_service_name(app_cfg.strat.strategy.type);

    try {
        return bpt::app::run(service_name, std::move(app_cfg),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                return std::make_unique<bpt::strategy::StrategyApp>(
                    std::move(cfg), ctx.aeron, ctx.topology);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
