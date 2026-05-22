load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Shared microstructure features. Pure logic, no Aeron/network.
# Consumed by bpt-strategy (inline) and bpt-features/python (pybind11).

cc_library(
    name = "features",
    srcs = [
        "src/ofi.cpp",
        "src/queue.cpp",
        "src/realized_vol.cpp",
    ],
    hdrs = glob(["include/features/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        # FairValueEstimator + QueueTracker consume OrderBookState
        # (lives in bpt-common as a cross-cutting domain type, not a
        # feature). QueueTracker also uses the OrderSide enum from
        # //messages.
        "//bpt-common",
        "//messages",
    ],
)

cc_test(
    name = "features_unit_tests",
    srcs = glob(["tests/test_*.cpp"]),
    deps = [
        ":features",
        "@googletest//:gtest_main",
    ],
)
