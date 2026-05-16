load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

cc_library(
    name = "pms_core",
    srcs = [
        "src/config/loader.cpp",
    ],
    hdrs = glob(["include/pms/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//messages",
        "//bpt-app",
        "//bpt-common",
        "@fmt",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_binary(
    name = "bpt-pms",
    srcs = [
        "src/main.cpp",
        "src/app/pms_service.cpp",
        "src/adapter/hyperliquid_balance_adapter.cpp",
        "src/messaging/aeron_bus.cpp",
        "src/messaging/publishers/balance_snapshot_publisher.cpp",
    ],
    deps = [
        ":pms_core",
        "//bpt-app",
        "//bpt-common",
        "@boost.asio",
        "@boost.beast",
        "@boost.json",
        "@boost.system",
        "@fmt",
        "@openssl",
        "@tomlplusplus//:tomlplusplus",
    ],
)

# Dev tool: subscribes to the BalanceSnapshot stream and prints each
# row. Used to verify bpt-pms's SBE encoding end-to-end without
# requiring the dashboard or a strategy to be running.
cc_binary(
    name = "balance_peek",
    srcs = ["tools/balance_peek.cpp"],
    deps = [
        "//messages",
        "//bpt-common",
        "@aeron//:aeron_client",
    ],
)
