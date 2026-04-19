load("@rules_cc//cc:cc_library.bzl", "cc_library")

# bpt-common — shared primitives (logging, signal, TSC clock, Aeron utils, secrets).
# Excludes ws_connect (needs Boost.Beast — keep per-service if they need it).
# Secrets client reads from $CREDENTIALS_DIRECTORY (systemd-creds).
cc_library(
    name = "bpt-common",
    srcs = [
        "src/logging.cpp",
        "src/secrets/secrets_client.cpp",
        "src/util/tsc_clock.cpp",
    ],
    hdrs = glob(["include/bpt_common/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "@quill",
        "@aeron//:aeron_client",
        "@fmt",
        "@tomlplusplus//:tomlplusplus",
    ],
    linkopts = ["-lpthread"],
)
