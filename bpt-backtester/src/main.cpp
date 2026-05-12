// Backtester — replay-driven backtest runner.

#include "backtester/app/backtester_app.h"
#include "backtester/config/settings.h"
#include "backtester/messaging/aeron_bus.h"

#include <CLI/CLI.hpp>
#include <bpt_app/app.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <memory>
#include <optional>
#include <string>

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-backtester — replay-driven backtest runner"};
    std::string config_path;
    std::optional<double> starting_capital_override;
    std::string strategy_name;
    std::string params_hash;
    std::string git_sha;
    std::string params_file;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")->required()->check(CLI::ExistingFile);
    cli.add_option("--starting-capital", starting_capital_override, "Override [results].starting_capital");
    cli.add_option("--strategy-name",
                   strategy_name,
                   "Strategy identity (e.g. AvellanedaStoikov) — recorded in summary.json + run_id");
    cli.add_option("--params-hash",
                   params_hash,
                   "sha256 of the strategy config file (orchestrator computes); 8+ chars used in run_id");
    cli.add_option("--git-sha", git_sha, "Repo HEAD SHA at run time; first 7 chars used in run_id");
    cli.add_option("--params-file",
                   params_file,
                   "Resolved strategy params toml — copied into the run dir as params.toml")
        ->check(CLI::ExistingFile);
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
    if (!strategy_name.empty())
        settings.results.strategy_name = std::move(strategy_name);
    if (!params_hash.empty())
        settings.results.params_hash = std::move(params_hash);
    if (!git_sha.empty())
        settings.results.git_sha = std::move(git_sha);
    if (!params_file.empty())
        settings.results.params_file = std::move(params_file);

    // Optional fault injection (dev/qa only). Must run before bpt::app::run
    // builds the AeronBus — Subscribers consult the registry at ctor time.
    try {
        bpt::common::aeron::install_chaos_from_toml(config_path,
                                                    bpt::common::to_string(settings.base.environment),
                                                    "bpt-backtester");
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-backtester");
        bpt::common::log::error("[chaos] config rejected: {}", e.what());
        return 1;
    }

    try {
        return bpt::app::run("bpt-backtester",
                             std::move(settings),
                             [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                                 bpt::common::log::info("starting_capital=${:.2f}", cfg.results.starting_capital);
                                 auto bus = bpt::backtester::messaging::BacktesterAeronBus::build(ctx.aeron, cfg);
                                 return std::make_unique<bpt::backtester::BacktesterApp>(std::move(cfg),
                                                                                         std::move(bus));
                             });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
