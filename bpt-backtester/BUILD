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
