load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
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
        "//bpt-app",
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

cc_binary(
    name = "bpt-pricer",
    srcs = [
        "src/main.cpp",
        "src/app/pricer_service.cpp",
        "src/config/config.cpp",
        "src/surface/surface_builder.cpp",
        "src/messaging/aeron_bus.cpp",
        "src/messaging/vol_surface_publisher.cpp",
        "src/messaging/status_publisher.cpp",
        "src/md/md_subscribe_client.cpp",
        "src/md/md_subscriber.cpp",
        "src/refdata/refdata_subscriber.cpp",
    ],
    deps = [
        ":pricer_core",
        "//bpt-app",
        "//bpt-common",
        "@fmt",
        "@tomlplusplus//:tomlplusplus",
    ],
)
