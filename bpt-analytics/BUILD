load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "analytics_core",
    srcs = [
        "src/analysis/fill_rate_tracker.cpp",
        "src/analysis/markout_tracker.cpp",
        "src/analysis/toxicity_scorer.cpp",
    ],
    hdrs = glob(["include/analytics/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
        "@fmt",
    ],
)

cc_test(
    name = "analytics_unit_tests",
    srcs = [
        "tests/unit/test_fill_rate_tracker.cpp",
        "tests/unit/test_markout_tracker.cpp",
        "tests/unit/test_toxicity_scorer.cpp",
    ],
    deps = [
        ":analytics_core",
        "@googletest//:gtest_main",
    ],
)

# Deployable binary (bpt-analytics) — test-friendly `analytics_core` plus the
# Aeron-coupled app + config loader.
cc_binary(
    name = "bpt-analytics",
    srcs = [
        "src/main.cpp",
        "src/app/analytics_service.cpp",
        "src/config/config.cpp",
        "src/messaging/aeron_bus.cpp",
        "src/messaging/subscribers/exec_report_subscriber.cpp",
        "src/messaging/subscribers/md_bbo_subscriber.cpp",
        "src/messaging/publishers/toxicity_publisher.cpp",
    ],
    deps = [
        ":analytics_core",
        "//bpt-app",
        "//bpt-common",
        "@tomlplusplus//:tomlplusplus",
    ],
)
