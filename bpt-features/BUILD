load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# bpt-features — shared microstructure-feature library.
#
# Pure C++ computations on market data (OFI, microprice, realized vol,
# queue position, ...). No Aeron, no network, no strategy logic — just
# the math.
#
# Consumed by:
#   - bpt-strategy: production strategies compute features inline
#   - bpt-features/python (planned): pybind11 bindings for research
#     notebooks. Same code runs in both paths so Python research can't
#     drift from C++ production.
#
# Each feature is a self-contained class with a `Config` struct and
# either a streaming update() method (typical) or a stateless compute()
# (rare — only for pure functions of one snapshot).
#
# Conventions:
#   - Namespace: bpt::features
#   - Header path: features/<name>.h
#   - One feature per (header, cpp) pair where possible
#   - Stdlib-only deps preferred; complex types (OrderBookState, etc.)
#     live here too when they're shared "feature input" domain types

cc_library(
    name = "features",
    srcs = [
        "src/ofi.cpp",
        "src/order_book_state.cpp",
        "src/queue.cpp",
        "src/realized_vol.cpp",
    ],
    hdrs = glob(["include/features/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        # OrderBookState consumes MdOrderBook; QueueTracker uses OrderSide enum.
        # Both are wire-format types from the shared messages package — not
        # strategy logic, safe to depend on here.
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
