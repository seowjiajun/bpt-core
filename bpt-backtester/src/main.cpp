// Backtester — replay-driven backtest runner.

#include "backtester/app/backtester_app.h"
#include "backtester/config/settings.h"

#include <CLI/CLI.hpp>
#include <memory>
#include <optional>
#include <string>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-backtester — replay-driven backtest runner"};
    std::string config_path;
    std::optional<double> starting_capital_override;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--starting-capital", starting_capital_override,
                   "Override [results].starting_capital");
    CLI11_PARSE(cli, argc, argv);

    bpt::backtester::config::Settings settings;
    try {
        settings = bpt::backtester::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-backtester");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    if (starting_capital_override) {
        settings.results.starting_capital = *starting_capital_override;
    }

    try {
        return bpt::app::run("bpt-backtester", std::move(settings),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                bpt::common::log::info("starting_capital=${:.2f}",
                                       cfg.results.starting_capital);
                return std::make_unique<bpt::backtester::BacktesterApp>(
                    std::move(cfg), ctx.aeron);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
