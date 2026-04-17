// Backtester — Backtest Exchange Simulator
// The world serpent that swallows the entire market history.

#include "backtester/app/backtester_app.h"
#include "backtester/config/settings.h"

#include <optional>
#include <string>
#include <yggdrasil/logging.h>
#include <yggdrasil/signal.h>

int main(int argc, char* argv[]) {
    ygg::signal::install();

    // Arg parsing: first non-flag positional is the config path.
    // Flags: --starting-capital <N>  (overrides [results] starting_capital)
    std::string config_path = "config/backtester.toml";
    std::optional<double> starting_capital_override;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--starting-capital" && i + 1 < argc) {
            try {
                starting_capital_override = std::stod(argv[i + 1]);
            } catch (const std::exception&) {
                // Ignore parse errors; default stays
            }
            ++i;
        } else if (!arg.empty() && arg[0] != '-') {
            config_path = std::move(arg);
        }
    }

    bpt::backtester::config::Settings settings;
    try {
        settings = bpt::backtester::config::load(config_path);
    } catch (const std::exception& e) {
        ygg::logging::init("bpt-backtester");
        ygg::log::error("[Backtester] Failed to load config: {}", e.what());
        return 1;
    }

    // CLI override takes precedence over TOML.
    if (starting_capital_override) {
        settings.results.starting_capital = *starting_capital_override;
    }

    ygg::logging::init("bpt-backtester", settings.logging);
    ygg::log::info("[Backtester] Starting — the world serpent awakens.");
    ygg::log::info("[Backtester] starting_capital=${:.2f}", settings.results.starting_capital);

    try {
        bpt::backtester::BacktesterApp app(std::move(settings));
        app.run();
    } catch (const std::exception& e) {
        ygg::log::error("[Backtester] Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
