load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

cc_library(
    name = "book_core",
    srcs = [
        "src/config/loader.cpp",
    ],
    hdrs = glob(["include/book/**/*.h"]),
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
    name = "bpt-book",
    srcs = [
        "src/main.cpp",
        "src/app/book_app.cpp",
        "src/adapter/hyperliquid_balance_adapter.cpp",
        "src/messaging/balance_snapshot_publisher.cpp",
    ],
    deps = [
        ":book_core",
        "//bpt-app",
        "//bpt-common",
        "@boost.asio",
        "@boost.beast",
        "@boost.json",
        "@boost.system",
        "@cli11",
        "@fmt",
        "@openssl",
        "@tomlplusplus//:tomlplusplus",
    ],
)

# Dev tool: subscribes to the BalanceSnapshot stream and prints each
# row. Used to verify bpt-book's SBE encoding end-to-end without
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
