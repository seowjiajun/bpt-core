load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "refdata_lib",
    srcs = [
        "src/mapping/instrument_mapping_loader.cpp",
        "src/mapping/instrument_mapping_merger.cpp",
        "src/registry/instrument_registry.cpp",
        "src/resolver/instrument_resolver.cpp",
        "src/messaging/refdata_control_subscriber.cpp",
        "src/messaging/refdata_snapshot_publisher.cpp",
        "src/messaging/refdata_delta_publisher.cpp",
        "src/messaging/subscription_manager.cpp",
        "src/messaging/fee_schedule_publisher.cpp",
        "src/messaging/refdata_status_publisher.cpp",
        "src/config/loader.cpp",
        "src/http/rest_client.cpp",
        "src/adapter/binance/binance_parser.cpp",
        "src/adapter/binance/binance_refdata_adapter.cpp",
        "src/adapter/okx/okx_parser.cpp",
        "src/adapter/okx/okx_refdata_auth.cpp",
        "src/adapter/okx/okx_refdata_adapter.cpp",
        "src/adapter/hyperliquid/hyperliquid_parser.cpp",
        "src/adapter/hyperliquid/hyperliquid_refdata_adapter.cpp",
        "src/adapter/deribit/deribit_parser.cpp",
        "src/adapter/deribit/deribit_refdata_encoder.cpp",
        "src/adapter/deribit/deribit_refdata_adapter.cpp",
        "src/metrics/metrics.cpp",
    ],
    hdrs = glob(["include/refdata/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
        "//bpt-app",
        "//bpt-common",
        "@boost.asio",
        "@boost.beast",
        "@boost.system",
        "@fmt",
        "@nlohmann_json//:nlohmann_json",
        "@openssl",
        "@prometheus_cpp//:core",
        "@prometheus_cpp//:pull",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_test(
    name = "refdata_tests",
    srcs = glob(["tests/*.cpp"]),
    data = ["//config/instruments:mapping_files"],
    deps = [
        ":refdata_lib",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bpt-refdata",
    srcs = [
        "src/main.cpp",
        "src/app/refdata_app.cpp",
    ],
    deps = [
        ":refdata_lib",
        "//bpt-app",
        "//bpt-common",
        "@cli11",
        "@fmt",
        "@tomlplusplus//:tomlplusplus",
    ],
)
