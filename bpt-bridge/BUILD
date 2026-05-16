load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# bpt-bridge: subscribes to Aeron streams, forwards as JSON over WebSocket.
# Sits at the intersection of every Aeron stream in the stack; the dashboard
# frontend in dashboard/frontend/ is its primary consumer over WS, but other
# consumers can subscribe to the same upstream streams independently.
cc_library(
    name = "bridge_lib",
    srcs = [
        "src/app/bridge_service.cpp",
        "src/config/config_loader.cpp",
        "src/messaging/aeron_bus.cpp",
        "src/messaging/publishers/dashboard_control_publisher.cpp",
        "src/messaging/subscribers/account_subscriber.cpp",
        "src/messaging/subscribers/exec_subscriber.cpp",
        "src/messaging/subscribers/market_color_subscriber.cpp",
        "src/messaging/subscribers/md_subscriber.cpp",
        "src/messaging/subscribers/portfolio_snapshot_subscriber.cpp",
        "src/messaging/subscribers/toxicity_subscriber.cpp",
        "src/state/position_tracker.cpp",
        "src/ws/message_encoder.cpp",
        "src/ws/ws_server.cpp",
    ],
    hdrs = glob(["include/bridge/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//bpt-analytics:analytics_core",
        "//bpt-radar:radar_messages",
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

# Seam tests: drive BridgeService event handlers directly via IBroadcaster /
# IDashboardControlSink fakes — no Aeron driver, no real WS listener.
cc_test(
    name = "bridge_seam_tests",
    srcs = ["tests/unit/test_bridge_service_seam.cpp"],
    deps = [
        ":bridge_lib",
        "@googletest//:gtest_main",
    ],
)
