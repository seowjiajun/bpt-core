load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Pure-logic strategy utilities — no Aeron, no network deps.
# Testable in isolation.
cc_library(
    name = "strategy_core",
    srcs = [
        "src/strategy/hurst_estimator.cpp",
        "src/strategy/ofi_calculator.cpp",
        "src/strategy/position_tracker.cpp",
        "src/strategy/regime_detector.cpp",
        "src/strategy/realized_vol_estimator.cpp",
        "src/refdata/instrument_cache.cpp",
    ],
    hdrs = glob(["include/strategy/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
        "//bpt-analytics:analytics_core",
    ],
)

cc_test(
    name = "strategy_unit_tests",
    srcs = [
        "tests/unit/test_instrument_cache.cpp",
        "tests/unit/test_ofi_calculator.cpp",
        "tests/unit/test_position_tracker.cpp",
        "tests/unit/test_regime_detector.cpp",
        "tests/unit/test_volatility_gate.cpp",
    ],
    deps = [
        ":strategy_core",
        "@googletest//:gtest_main",
    ],
)
