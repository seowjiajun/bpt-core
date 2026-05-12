/// bpt-backtester-mono — single-process deterministic backtest entry.
///
/// Replaces the multi-process backtest stack as the parameter-tuning
/// device. Same strategy code, same config, but every event flows
/// through a single thread — no Aeron, no IPC scheduler jitter.
///
/// The multi-process stack (scripts/backtest.sh) remains as the
/// integration smoke test for the live wire; this binary is the
/// measurement device.

#include "backtester/harness/strategy_harness.h"

#include <CLI/CLI.hpp>
#include <bpt_common/logging.h>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-backtester-mono — deterministic single-process backtest"};

    std::string strategy_config;
    std::string instrument_mapping;
    std::vector<std::string> wslog_paths;
    std::string output_dir = "results";
    double starting_capital = 1000.0;
    std::string strategy_name;
    std::string params_hash;
    std::string git_sha;

    cli.add_option("--strategy-config", strategy_config, "Path to strategy TOML")->required()->check(CLI::ExistingFile);
    cli.add_option("--instrument-mapping", instrument_mapping, "Path to instrument_mapping.<venue>.json")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--wslog", wslog_paths, "One or more .wslog files to replay (in timestamp order)")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--output-dir", output_dir, "Where to write results")->capture_default_str();
    cli.add_option("--starting-capital", starting_capital, "Starting capital ($) — feeds ResultsCollector")
        ->capture_default_str();
    cli.add_option("--strategy-name",
                   strategy_name,
                   "Strategy identity (e.g. AvellanedaStoikov) — recorded in summary.json");
    cli.add_option("--params-hash", params_hash, "sha256 of the strategy config; first 8 chars used in run_id");
    cli.add_option("--git-sha", git_sha, "Repo HEAD SHA at run time; first 7 chars used in run_id");

    CLI11_PARSE(cli, argc, argv);

    bpt::common::logging::init("bpt-backtester-mono");

    bpt::backtester::harness::StrategyHarness::Options opts{
        .strategy_config_path = strategy_config,
        .instrument_mapping_path = instrument_mapping,
        .wslog_paths = wslog_paths,
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
