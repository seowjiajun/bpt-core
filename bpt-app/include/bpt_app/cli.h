#pragma once

/// @file
/// Shared CLI entry point for every bpt-* binary. Replaces the eight
/// near-identical CLI11 boilerplate blocks that used to live in each
/// service's main.cpp with a single function call.
///
/// Adding a new common flag (e.g. --env-override, --dry-run, --print-config)
/// is now a one-file change here instead of a coordinated edit across
/// every binary. The CLI11 dependency is also confined to cli.cpp —
/// callers don't need to include it.
///
/// Per-binary custom flags can be added by passing an `extra` callback
/// that receives the underlying CLI::App; default is no-op so the 90%
/// case (a single --config flag) stays trivially short:
///
///   auto args = bpt::app::parse_cli(argc, argv,
///                                   "bpt-tape",
///                                   "venue WS-frame recorder");

#include <functional>
#include <string>
#include <string_view>

namespace CLI {
class App;
}

namespace bpt::app {

struct CliArgs {
    /// Path to the service TOML config. `--config` / `-c`. Required and
    /// must exist on disk — parser exits 1 if either condition is
    /// violated. Always populated by the time parse_cli() returns.
    std::string config_path;
};

/// Parse argv. Behaves exactly like CLI11_PARSE on error / --help /
/// --version: prints the appropriate message and calls std::exit().
/// Successful parses return the populated CliArgs.
///
/// `extra` runs against the CLI::App after the standard flags are
/// declared but before parse(), so per-binary options can be added
/// without forking the wrapper. Pass {} for the common case.
CliArgs parse_cli(int argc,
                  char** argv,
                  std::string_view program,
                  std::string_view description,
                  std::function<void(CLI::App&)> extra = {});

}  // namespace bpt::app
