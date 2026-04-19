load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

# Dashboard bridge: subscribes to Aeron streams, forwards as JSON over WebSocket.
# No unit tests exist in the source tree — build the lib + app binary only.
cc_library(
    name = "bridge_lib",
    srcs = [
        "src/message_encoder.cpp",
        "src/position_tracker.cpp",
        "src/ws_server.cpp",
        "src/account_subscriber.cpp",
        "src/exec_subscriber.cpp",
        "src/md_subscriber.cpp",
        "src/config_loader.cpp",
    ],
    hdrs = glob(["include/bridge/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//bpt-analytics:analytics_core",
        "//messages",
        "//bpt-common",
        "@boost.asio",
        "@boost.beast",
        "@boost.system",
        "@fmt",
        "@nlohmann_json//:nlohmann_json",
        "@tomlplusplus//:tomlplusplus",
    ],
)

cc_binary(
    name = "bridge",
    srcs = ["src/main.cpp"],
    deps = [
        ":bridge_lib",
        "@cli11",
    ],
)
