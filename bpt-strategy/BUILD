load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Pure-logic strategy utilities — no Aeron, no network deps.
# Testable in isolation.
cc_library(
    name = "strategy_core",
    srcs = [
        "src/strategy/hurst_estimator.cpp",
        "src/strategy/ofi_calculator.cpp",
        "src/strategy/order_book_state.cpp",
        "src/strategy/position_tracker.cpp",
        "src/strategy/queue_tracker.cpp",
        "src/strategy/reconciler.cpp",
        "src/strategy/regime_detector.cpp",
        "src/strategy/realized_vol_estimator.cpp",
        "src/refdata/instrument_cache.cpp",
    ],
    hdrs = glob(["include/strategy/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
        "//bpt-app",
        "//bpt-analytics:analytics_core",
    ],
)

cc_test(
    name = "strategy_unit_tests",
    srcs = [
        "tests/unit/test_fair_value_estimator.cpp",
        "tests/unit/test_instrument_cache.cpp",
        "tests/unit/test_ofi_calculator.cpp",
        "tests/unit/test_order_book_state.cpp",
        "tests/unit/test_position_tracker.cpp",
        "tests/unit/test_queue_tracker.cpp",
        "tests/unit/test_reconciler.cpp",
        "tests/unit/test_refdata_stale_gate.cpp",
        "tests/unit/test_regime_detector.cpp",
        "tests/unit/test_volatility_gate.cpp",
    ],
    deps = [
        ":strategy_core",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bpt-strategy",
    srcs = [
        "src/main.cpp",
        "src/app/strategy_app.cpp",
        "src/app/strategy_app_wiring.cpp",
        "src/app/strategy_app_shutdown.cpp",
        "src/app/strategy_app_backtest.cpp",
        "src/app/startup_gate.cpp",
        "src/backtest/backtest_client.cpp",
        "src/config/config.cpp",
        "src/dashboard/portfolio_snapshot_publisher.cpp",
        "src/messaging/aeron_bus.cpp",
        "src/messaging/dashboard_control_subscriber.cpp",
        "src/messaging/toxicity_subscriber.cpp",
        "src/metrics/metrics.cpp",
        "src/refdata/refdata_client.cpp",
        "src/refdata/fee_cache.cpp",
        "src/refdata/funding_rate_cache.cpp",
        "src/strategy/canonical_resolver.cpp",
        "src/strategy/hmm_filter.cpp",
        "src/strategy/hmm_strategy.cpp",
        "src/strategy/momentum_strategy.cpp",
        "src/strategy/ofi_strategy.cpp",
        "src/strategy/avellaneda_stoikov_strategy.cpp",
        "src/strategy/funding_arb_strategy.cpp",
        "src/strategy/regime_switch_strategy.cpp",
        "src/strategy/vwap_reversion_strategy.cpp",
        "src/strategy/short_vol_strategy.cpp",
        "src/strategy/strategy_factory.cpp",
        "src/md/md_client.cpp",
        "src/vol/vol_surface_client.cpp",
        "src/order/aeron_order_gateway_client.cpp",
        "src/order/order_manager.cpp",
    ],
    deps = [
        ":strategy_core",
        "//bpt-app",
        "//bpt-common",
        "@cli11",
        "@fmt",
        "@nlohmann_json//:nlohmann_json",
        "@prometheus_cpp//:core",
        "@prometheus_cpp//:pull",
        "@tomlplusplus//:tomlplusplus",
    ],
)
