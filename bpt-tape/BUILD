load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

# bpt-tape — venue WS-frame recorder ("the tape"). Imports bpt-md-gateway's
# adapter library and substitutes recording subclasses that tee raw bytes
# to disk via bpt::common::recorder::RawSpool. mdgw + refdata service
# binaries are untouched; recording lives entirely in this process.
# Refdata REST snapshot capture is a planned extension via the same RawSpool.

cc_library(
    name = "tape_lib",
    srcs = [
        "src/app/recorder_service.cpp",
        "src/config/loader.cpp",
        "src/metrics/metrics.cpp",
        "src/refdata/refdata_poller.cpp",
    ],
    hdrs = glob(["include/tape/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//bpt-app",
        "//bpt-common",
        "//bpt-md-gateway:md_gateway_lib",
        "//bpt-refdata:mapping_lib",
        "//bpt-refdata:rest_client_lib",
        "@fmt",
        "@prometheus_cpp//:core",
        "@prometheus_cpp//:pull",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_binary(
    name = "bpt-tape",
    srcs = [
        "src/main.cpp",
    ],
    deps = [
        ":tape_lib",
        "//bpt-app",
        "//bpt-common",
        "//bpt-md-gateway:md_gateway_lib",
        "//bpt-refdata:mapping_lib",
        "//bpt-refdata:rest_client_lib",
    ],
)
