load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Header-only domain message struct. Tiny target so consumers (e.g. bpt-bridge)
# can include `radar/messaging/market_color.h` without pulling in Aeron deps
# they don't need.
cc_library(
    name = "radar_messages",
    hdrs = ["include/radar/messaging/market_color.h"],
    includes = ["include"],
    visibility = ["//visibility:public"],
)

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
        "src/messaging/publishers/market_color_publisher.cpp",
        "src/messaging/subscribers/funding_rate_subscriber.cpp",
        "src/messaging/subscribers/instrument_stats_subscriber.cpp",
        "src/messaging/subscribers/md_market_data_subscriber.cpp",
        "src/messaging/subscribers/md_trade_subscriber.cpp",
        "src/messaging/subscribers/refdata_perp_subscriber.cpp",
        "src/messaging/subscribers/vol_surface_subscriber.cpp",
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
