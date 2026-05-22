load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Pure-logic strategy utilities — no Aeron, no network deps.
# Testable in isolation.
cc_library(
    name = "strategy_core",
    srcs = [
        "src/strategy/canonical_resolver.cpp",
        "src/strategy/hurst_estimator.cpp",
        "src/strategy/position_tracker.cpp",
        "src/strategy/reconciler.cpp",
        "src/strategy/regime_detector.cpp",
        "src/refdata/instrument_cache.cpp",
    ],
    hdrs = glob(["include/strategy/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//bpt-features:features",
        "//messages",
        "//bpt-app",
        "//bpt-analytics:analytics_core",
    ],
)

cc_test(
    name = "strategy_unit_tests",
    srcs = [
        "tests/unit/test_canonical_resolver.cpp",
        "tests/unit/test_exchange_id.cpp",
        "tests/unit/test_instrument_cache.cpp",
        "tests/unit/test_position_tracker.cpp",
        "tests/unit/test_reconciler.cpp",
        "tests/unit/test_refdata_stale_gate.cpp",
        "tests/unit/test_passive_maker.cpp",
        "tests/unit/test_regime_classifier.cpp",
        "tests/unit/test_regime_detector.cpp",
        "tests/unit/test_sim_clock.cpp",
    ],
    deps = [
        ":strategy_core",
        "@googletest//:gtest_main",
    ],
)

# Full strategy library — every concrete strategy class plus the app
# scaffolding (refdata client, MD client, order manager, lifecycle).
# Split out from `:bpt-strategy` cc_binary so a deterministic backtest
# harness (in bpt-backtester) can link AvellanedaStoikovStrategy +
# friends without duplicating the source list. The cc_binary now just
# wraps main.cpp around this library.
cc_library(
    name = "strategy_lib",
    srcs = [
        "src/app/strategy_service.cpp",
        "src/app/strategy_service_wiring.cpp",
        "src/app/strategy_service_shutdown.cpp",
        "src/app/strategy_service_backtest.cpp",
        "src/app/startup_gate.cpp",
        "src/backtest/backtest_client.cpp",
        "src/config/config.cpp",
        "src/console/portfolio_snapshot_publisher.cpp",
        "src/messaging/aeron_bus.cpp",
        "src/metrics/metrics.cpp",
        "src/refdata/fee_cache.cpp",
        "src/refdata/funding_rate_cache.cpp",
        "src/strategy/hmm_filter.cpp",
        "src/strategy/hmm_strategy.cpp",
        "src/strategy/momentum_strategy.cpp",
        "src/strategy/ofi_strategy.cpp",
        "src/strategy/passive_maker_strategy.cpp",
        "src/strategy/avellaneda_stoikov_strategy.cpp",
        "src/strategy/avellaneda_stoikov_quoting.cpp",
        "src/strategy/avellaneda_stoikov_shutdown.cpp",
        "src/strategy/avellaneda_stoikov_state_io.cpp",
        "src/strategy/funding_arb_strategy.cpp",
        "src/strategy/regime_switch_strategy.cpp",
        "src/strategy/vwap_reversion_strategy.cpp",
        "src/strategy/short_vol_strategy.cpp",
        "src/strategy/options_maker_strategy.cpp",
        "src/strategy/strategy_factory.cpp",
        "src/order/order_manager.cpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":strategy_core",
        "//bpt-app",
        "//bpt-common",
        "//bpt-pricer:pricer_core",
        "@fmt",
        "@nlohmann_json//:nlohmann_json",
        "@prometheus_cpp//:core",
        "@prometheus_cpp//:pull",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_binary(
    name = "bpt-strategy",
    srcs = ["src/main.cpp"],
    deps = [":strategy_lib"],
)
