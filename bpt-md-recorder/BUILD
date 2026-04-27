load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

# bpt-md-recorder — venue WS-frame recorder. Imports bpt-md-gateway's adapter
# library and substitutes recording subclasses that tee raw bytes to disk via
# bpt::common::recorder::RawSpool. mdgw + refdata service binaries are
# untouched; recording lives entirely in this process.

cc_library(
    name = "md_recorder_lib",
    srcs = [
        "src/config/loader.cpp",
    ],
    hdrs = glob(["include/md_recorder/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//bpt-app",
        "//bpt-common",
        "//bpt-md-gateway:md_gateway_lib",
        "@fmt",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_binary(
    name = "bpt-md-recorder",
    srcs = [
        "src/main.cpp",
    ],
    deps = [
        ":md_recorder_lib",
        "//bpt-app",
        "//bpt-common",
        "//bpt-md-gateway:md_gateway_lib",
        "@cli11",
        "@fmt",
    ],
)
