// Backtester — Backtest Exchange Simulator
// The world serpent that swallows the entire market history.

#include "backtester/app/backtester_app.h"
#include "backtester/config/settings.h"

#include <CLI/CLI.hpp>
#include <optional>
#include <string>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>

int main(int argc, char* argv[]) {
    bpt::common::signal::install();

    CLI::App app{"bpt-backtester — replay-driven backtest runner"};
    std::string config_path = "config/backtester.toml";
    std::optional<double> starting_capital_override;
    app.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    app.add_option("--starting-capital", starting_capital_override,
                   "Override [results].starting_capital");
    CLI11_PARSE(app, argc, argv);

    bpt::backtester::config::Settings settings;
    try {
        settings = bpt::backtester::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-backtester");
        bpt::common::log::error("[Backtester] Failed to load config: {}", e.what());
        return 1;
    }

    // CLI override takes precedence over TOML.
    if (starting_capital_override) {
        settings.results.starting_capital = *starting_capital_override;
    }

    bpt::common::logging::init("bpt-backtester", settings.logging);
    bpt::common::log::info("[Backtester] Starting — the world serpent awakens.");
    bpt::common::log::info("[Backtester] starting_capital=${:.2f}", settings.results.starting_capital);

    try {
        bpt::backtester::BacktesterApp app(std::move(settings));
        app.run();
    } catch (const std::exception& e) {
        bpt::common::log::error("[Backtester] Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
