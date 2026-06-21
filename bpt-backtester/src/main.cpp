/// bpt-backtester — single-process deterministic backtest entry.
///
/// Strategy code is linked directly to the matching engine via the
/// InProcess client classes; every event flows through a single thread —
/// no Aeron, no IPC scheduler jitter. Same strategy code paths as live
/// (just a different transport at the bus boundary), so behaviour
/// transfers verbatim.

#include "backtester/harness/strategy_harness.h"

#include <CLI/CLI.hpp>
#include <bpt_common/logging.h>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-backtester — deterministic single-process backtest"};

    std::string strategy_config;
    std::string instrument_mapping;
    std::vector<std::string> wslog_paths;
    std::vector<std::string> canon_paths;
    std::string output_dir = "results";
    double starting_capital = 1000.0;
    std::string strategy_name;
    std::string params_hash;
    std::string git_sha;

    cli.add_option("--strategy-config", strategy_config, "Path to strategy TOML")->required()->check(CLI::ExistingFile);
    cli.add_option("--instrument-mapping", instrument_mapping, "Path to instrument_mapping.<venue>.json")
        ->required()
        ->check(CLI::ExistingFile);
    auto* wslog_opt = cli.add_option("--wslog", wslog_paths, "One or more .wslog files to replay (raw venue capture)")
                          ->check(CLI::ExistingFile);
    auto* canon_opt = cli.add_option("--canon", canon_paths, "One or more .canon files to replay (derived SBE events)")
                          ->check(CLI::ExistingFile);
    wslog_opt->excludes(canon_opt);
    canon_opt->excludes(wslog_opt);
    cli.add_option("--output-dir", output_dir, "Where to write results")->capture_default_str();
    cli.add_option("--starting-capital", starting_capital, "Starting capital ($) — feeds ResultsCollector")
        ->capture_default_str();
    cli.add_option("--strategy-name",
                   strategy_name,
                   "Strategy identity (e.g. AvellanedaStoikov) — recorded in summary.json");
    cli.add_option("--params-hash", params_hash, "sha256 of the strategy config; first 8 chars used in run_id");
    cli.add_option("--git-sha", git_sha, "Repo HEAD SHA at run time; first 7 chars used in run_id");

    CLI11_PARSE(cli, argc, argv);

    bpt::common::logging::init("bpt-backtester");

    if (wslog_paths.empty() && canon_paths.empty()) {
        std::cerr << "fatal: exactly one of --wslog or --canon is required\n";
        return 1;
    }

    bpt::backtester::harness::StrategyHarness::Options opts{
        .strategy_config_path = strategy_config,
        .instrument_mapping_path = instrument_mapping,
        .wslog_paths = wslog_paths,
        .canon_paths = canon_paths,
        .starting_capital = starting_capital,
        .output_dir = output_dir,
        .strategy_name = strategy_name,
        .params_hash = params_hash,
        .git_sha = git_sha,
    };

    try {
        bpt::backtester::harness::StrategyHarness harness(std::move(opts));
        harness.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
