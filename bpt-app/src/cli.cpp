#include "bpt_app/cli.h"

#include <CLI/CLI.hpp>
#include <fmt/format.h>

namespace bpt::app {

namespace {

// Build SHA / version. Bazel workspace-status integration is a separate
// follow-up (see backlog). For now this is a literal — at least gives
// `--version` a sensible answer and a place to update.
constexpr const char* kVersion = "0.1.0+dev";

}  // namespace

CliArgs parse_cli(int argc, char** argv,
                  std::string_view program,
                  std::string_view description,
                  std::function<void(CLI::App&)> extra) {
    CLI::App cli{fmt::format("{} — {}", program, description)};

    CliArgs args;
    cli.add_option("-c,--config", args.config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    cli.set_version_flag("--version", kVersion);

    if (extra) extra(cli);

    try {
        cli.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // CLI11's idiomatic exit path — handles --help, --version, and
        // parse errors with the right message + exit code.
        std::exit(cli.exit(e));
    }
    return args;
}

}  // namespace bpt::app
