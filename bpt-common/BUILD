load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# bpt-common — shared primitives (logging, signal, TSC clock, Aeron utils, secrets).
# Excludes ws_connect (needs Boost.Beast — keep per-service if they need it).
# Secrets client reads from $CREDENTIALS_DIRECTORY (systemd-creds).
cc_library(
    name = "bpt-common",
    srcs = [
        "src/aeron/streams_map.cpp",
        "src/book/order_book_state.cpp",
        "src/config/profile_config.cpp",
        "src/env.cpp",
        "src/logging.cpp",
        "src/secrets/secrets_client.cpp",
        "src/util/openssl_helpers.cpp",
        "src/util/topology.cpp",
        "src/util/tsc_clock.cpp",
    ],
    hdrs = glob(["include/bpt_common/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        # OrderBookState applies MdOrderBook deltas — wire-format type
        # from the shared messages package.
        "//messages",
        "@quill",
        "@aeron//:aeron_client",
        "@fmt",
        "@openssl",
        "@tomlplusplus//:tomlplusplus",
    ],
    # openssl symbols pulled in via openssl_helpers.cpp need explicit
    # link flags here so cc_test consumers of :bpt-common get them too —
    # @openssl as a deps entry alone doesn't propagate the libs to the
    # test binary's link command on this toolchain.
    linkopts = [
        "-lpthread",
        "-lssl",
        "-lcrypto",
    ],
)

cc_test(
    name = "bpt_common_unit_tests",
    srcs = glob(["tests/unit/*.cpp"]),
    deps = [
        ":bpt-common",
        "@googletest//:gtest_main",
    ],
)
