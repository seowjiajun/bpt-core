load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "pricer_core",
    srcs = [
        "src/pricing/black_scholes.cpp",
        "src/pricing/forward_curve.cpp",
        "src/pricing/iv_solver.cpp",
    ],
    hdrs = glob(["include/pricer/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
    ],
)

cc_test(
    name = "pricer_tests",
    srcs = [
        "tests/test_black_scholes.cpp",
        "tests/test_forward_curve.cpp",
        "tests/test_iv_solver.cpp",
    ],
    deps = [
        ":pricer_core",
        "@googletest//:gtest_main",
    ],
)
