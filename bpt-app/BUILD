load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# bpt-app — shared application skeleton. Every bpt-core service's main.cpp
# calls bpt::app::run<Settings>(...) to get a consistent lifecycle:
# signal install, TSC calibrate, logging init, Aeron connect, main loop,
# graceful shutdown. Services only contribute their own Settings struct
# and a build callable that constructs an IService.
cc_library(
    name = "bpt-app",
    srcs = [
        "src/base_settings.cpp",
        "src/cli.cpp",
    ],
    hdrs = glob(["include/bpt_app/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//bpt-common",
        "@aeron//:aeron_client",
        "@cli11",
        "@fmt",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_test(
    name = "bpt_app_unit_tests",
    srcs = glob(["tests/unit/*.cpp"]),
    deps = [
        ":bpt-app",
        "@googletest//:gtest_main",
    ],
)
