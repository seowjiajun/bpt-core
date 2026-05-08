load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# NOTE: `data/data_loader.cpp` + `clock/clock_master.cpp` + `exchange/*` +
# `messaging/*` + `app/backtester_app.cpp` + `main.cpp` are NOT included —
# they pull in Arrow/Parquet (no BCR module, rules_foreign_cc CMake build of
# Arrow is multi-GB and slow) or the Aeron-coupled messaging layer. The three
# testable pieces (config, matching, results_collector) don't touch any of
# that, so this narrower lib covers the unit tests that don't need Parquet.
cc_library(
    name = "backtester_core",
    srcs = [
        "src/config/loader.cpp",
        "src/matching/matching_engine.cpp",
        "src/results/results_collector.cpp",
    ],
    hdrs = glob(["include/backtester/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
        "//bpt-app",
        "//bpt-common",
        "@boost.json",
        "@fmt",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_test(
    name = "backtester_unit_tests",
    srcs = [
        "tests/unit/test_config.cpp",
        "tests/unit/test_matching_engine.cpp",
        "tests/unit/test_results_collector.cpp",
    ],
    deps = [
        ":backtester_core",
        "@googletest//:gtest_main",
    ],
)

# Deterministic single-process backtest harness. Wires the strategy
# library directly to the matching engine via the InProcess clients —
# no Aeron, no WS server, no IPC. Replaces the multi-process backtest
# stack as a *measurement device* for parameter sweeps; multi-process
# stack remains as the integration smoke for the live wire.
#
# Reads .wslog files directly (skipping the Arrow/Parquet path that
# keeps the rest of bpt-backtester on CMake) — same recorder format
# bpt-tape produces, parsed via the existing mdgw venue decoders.
cc_library(
    name = "harness_lib",
    srcs = [
        "src/harness/inprocess_order_gateway_client.cpp",
        "src/harness/strategy_harness.cpp",
    ],
    hdrs = glob(["include/backtester/harness/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        ":backtester_core",
        "//bpt-strategy:strategy_lib",
        "//bpt-md-gateway:md_gateway_lib",
        "//bpt-refdata:mapping_lib",
        "//bpt-common",
        "//messages",
    ],
)

cc_binary(
    name = "bpt-backtester-mono",
    srcs = ["src/main_mono.cpp"],
    deps = [
        ":harness_lib",
        "@cli11",
    ],
)
