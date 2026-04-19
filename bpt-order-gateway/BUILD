load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Core order-gateway components needed by tests — no network adapters.
# OrderProcessor lives here (not in the binary srcs) because it depends
# only on the IExecReportPublisher interface, so tests can drop in a
# capturing fake without bringing up Aeron.
cc_library(
    name = "order_gateway_core",
    srcs = [
        "src/config/loader.cpp",
        "src/metrics/metrics.cpp",
        "src/order/order_processor.cpp",
        "src/order/order_state_manager.cpp",
        "src/risk/disconnect_rate_breaker.cpp",
        "src/risk/pnl_tracker.cpp",
        "src/risk/reject_rate_breaker.cpp",
        "src/risk/risk_checker.cpp",
        "src/adapter/hyperliquid/hyperliquid_action_codec.cpp",
        "src/adapter/hyperliquid/hyperliquid_exec_parser.cpp",
        "src/adapter/hyperliquid/hyperliquid_reconciler.cpp",
        "src/adapter/hyperliquid/hyperliquid_signer.cpp",
    ],
    hdrs = glob(["include/order_gateway/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
        "//bpt-app",
        "//bpt-common",
        "@boost.json",
        "@fmt",
        "@openssl",
        "@prometheus_cpp//:core",
        "@prometheus_cpp//:pull",
        "@secp256k1//:secp256k1",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_test(
    name = "order_gateway_unit_tests",
    srcs = glob(["tests/unit/*.cpp"]),
    deps = [
        ":order_gateway_core",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bpt-order-gateway",
    srcs = [
        "src/main.cpp",
        "src/app/gateway_app.cpp",
        "src/messaging/account_snapshot_publisher.cpp",
        "src/messaging/exec_report_publisher.cpp",
        "src/messaging/heartbeat_publisher.cpp",
        "src/messaging/order_subscriber.cpp",
        "src/refdata/instrument_cache.cpp",
        "src/refdata/refdata_client.cpp",
        "src/adapter/common/beast_https_client.cpp",
        "src/adapter/common/order_adapter_base.cpp",
        "src/adapter/binance/binance_action_codec.cpp",
        "src/adapter/binance/binance_auth.cpp",
        "src/adapter/binance/binance_https_client.cpp",
        "src/adapter/binance/binance_user_data_ws.cpp",
        "src/adapter/binance/binance_order_adapter.cpp",
        "src/adapter/binance/binance_exec_parser.cpp",
        "src/adapter/okx/okx_action_codec.cpp",
        "src/adapter/okx/okx_auth.cpp",
        "src/adapter/okx/okx_https_client.cpp",
        "src/adapter/okx/okx_instruments_service.cpp",
        "src/adapter/okx/okx_ws_client.cpp",
        "src/adapter/okx/okx_order_adapter.cpp",
        "src/adapter/okx/okx_exec_parser.cpp",
        "src/adapter/deribit/deribit_action_codec.cpp",
        "src/adapter/deribit/deribit_ws_client.cpp",
        "src/adapter/deribit/deribit_order_adapter.cpp",
        "src/adapter/deribit/deribit_exec_parser.cpp",
        "src/adapter/hyperliquid/hyperliquid_exec_emitter.cpp",
        "src/adapter/hyperliquid/hyperliquid_https_client.cpp",
        "src/adapter/hyperliquid/hyperliquid_ws_client.cpp",
        "src/adapter/hyperliquid/hyperliquid_order_adapter.cpp",
    ],
    deps = [
        ":order_gateway_core",
        "//bpt-app",
        "//bpt-common",
        "@boost.asio",
        "@boost.beast",
        "@boost.json",
        "@boost.system",
        "@cli11",
        "@fmt",
        "@openssl",
        "@prometheus_cpp//:core",
        "@prometheus_cpp//:pull",
        "@tomlplusplus//:tomlplusplus",
    ],
)
