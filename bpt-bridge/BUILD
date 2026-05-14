load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

# bpt-bridge: subscribes to Aeron streams, forwards as JSON over WebSocket.
# Sits at the intersection of every Aeron stream in the stack; the dashboard
# frontend in dashboard/frontend/ is its primary consumer over WS, but other
# consumers can subscribe to the same upstream streams independently.
# No unit tests exist in the source tree — build the lib + app binary only.
cc_library(
    name = "bridge_lib",
    srcs = [
        "src/aeron/account_subscriber.cpp",
        "src/aeron/exec_subscriber.cpp",
        "src/aeron/md_subscriber.cpp",
        "src/app/bridge_service.cpp",
        "src/config/config_loader.cpp",
        "src/state/position_tracker.cpp",
        "src/ws/message_encoder.cpp",
        "src/ws/ws_server.cpp",
    ],
    hdrs = glob(["include/bridge/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//bpt-analytics:analytics_core",
        "//messages",
        "//bpt-app",
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
    name = "bpt-bridge",
    srcs = ["src/main.cpp"],
    deps = [
        ":bridge_lib",
        "//bpt-app",
        "//bpt-common",
        "@cli11",
    ],
)
