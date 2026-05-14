load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Pure-logic analysis modules — no Aeron, testable in isolation.
cc_library(
    name = "radar_core",
    srcs = [
        "src/analysis/gex.cpp",
        "src/analysis/max_pain.cpp",
        "src/analysis/surface_analyzer.cpp",
    ],
    hdrs = glob(["include/radar/analysis/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "radar_unit_tests",
    srcs = glob(["tests/unit/*.cpp"]),
    deps = [
        ":radar_core",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "radar_lib",
    srcs = [
        "src/app/radar_service.cpp",
        "src/config/config.cpp",
        "src/messaging/aeron_bus.cpp",
        "src/messaging/instrument_stats_subscriber.cpp",
        "src/messaging/market_color_publisher.cpp",
        "src/messaging/vol_surface_subscriber.cpp",
    ],
    hdrs = glob(["include/radar/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        ":radar_core",
        "//bpt-app",
        "//bpt-common",
        "//messages",
        "@fmt",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_binary(
    name = "bpt-radar",
    srcs = ["src/main.cpp"],
    deps = [
        ":radar_lib",
        "//bpt-app",
        "//bpt-common",
    ],
)
