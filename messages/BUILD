load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "messages",
    hdrs = glob(["generated/cpp/messages/*.h"]),
    includes = ["generated/cpp"],
    visibility = ["//visibility:public"],
)
