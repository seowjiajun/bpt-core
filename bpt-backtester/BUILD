load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Pure-logic pieces of the deterministic backtest harness: config loader,
# matching engine, results collector. No Aeron, no transport — testable
# in isolation.
cc_library(
    name = "backtester_core",
    srcs = [
        "src/matching/matching_engine.cpp",
        "src/matching/matching_engine_book.cpp",
        "src/matching/matching_engine_fills.cpp",
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
    ],
)

cc_test(
    name = "backtester_unit_tests",
    srcs = [
        "tests/unit/test_matching_engine.cpp",
        "tests/unit/test_results_collector.cpp",
    ],
    deps = [
        ":backtester_core",
        "@googletest//:gtest_main",
    ],
)

# Strategy harness — wires the strategy library directly to the matching
# engine via the InProcess clients. Reads .wslog tape produced by
# bpt-tape; the existing md-gateway venue decoders parse frames inline.
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
        "//bpt-canon:bpt-canon",
        "//bpt-canon:canon_sbe",
        "//bpt-strategy:strategy_lib",
        "//bpt-md-gateway:md_gateway_lib",
        "//bpt-refdata:mapping_lib",
        "//bpt-common",
        "//messages",
    ],
)

cc_binary(
    name = "bpt-backtester",
    srcs = ["src/main.cpp"],
    deps = [
        ":harness_lib",
        "@cli11",
    ],
)
