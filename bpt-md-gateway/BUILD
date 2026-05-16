load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "md_gateway_lib",
    srcs = [
        "src/config/loader.cpp",
        "src/md/md_encoder.cpp",
        "src/md/md_validator.cpp",
        "src/md/validation_drop_breaker.cpp",
        "src/app/md_gateway_service.cpp",
        "src/messaging/aeron_bus.cpp",
        "src/messaging/subscribers/md_control_subscriber.cpp",
        "src/messaging/publishers/md_publisher.cpp",
        "src/messaging/publishers/ack_publisher.cpp",
        "src/messaging/publishers/funding_rate_publisher.cpp",
        "src/messaging/publishers/instrument_stats_publisher.cpp",
        "src/adapter/common/subscription_map.cpp",
        "src/adapter/binance/binance_md_encoder.cpp",
        "src/adapter/binance/binance_funding_rate_stream.cpp",
        "src/adapter/okx/okx_md_encoder.cpp",
        "src/adapter/okx/okx_md_ws_client.cpp",
        "src/adapter/deribit/deribit_md_encoder.cpp",
        "src/adapter/deribit/deribit_md_ws_client.cpp",
        "src/adapter/hyperliquid/hyperliquid_md_encoder.cpp",
        "src/adapter/hyperliquid/hyperliquid_md_ws_client.cpp",
        "src/subscription/subscription_manager.cpp",
        "src/metrics/metrics.cpp",
    ],
    hdrs = glob(["include/md_gateway/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
        "//bpt-app",
        "//bpt-common",
        "@boost.asio",
        "@boost.beast",
        "@boost.system",
        "@cli11",
        "@fast_float//:fast_float",
        "@fmt",
        "@openssl",
        "@prometheus_cpp//:core",
        "@prometheus_cpp//:pull",
        "@simdjson//:simdjson",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_test(
    name = "md_gateway_unit_tests",
    srcs = glob(["tests/unit/*.cpp"]),
    deps = [
        ":md_gateway_lib",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bpt-md-gateway",
    srcs = [
        "src/main.cpp",
    ],
    deps = [
        ":md_gateway_lib",
        "//bpt-app",
        "//bpt-common",
    ],
)

cc_test(
    name = "md_gateway_component_tests",
    srcs = glob([
        "tests/component/*.cpp",
        "tests/component/*.h",
    ]),
    includes = ["tests/component"],
    deps = [
        ":md_gateway_lib",
        "@googletest//:gtest_main",
    ],
)
