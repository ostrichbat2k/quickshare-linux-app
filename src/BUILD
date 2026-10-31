load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "send_session",
    srcs = ["send_session.cc"],
    hdrs = ["send_session.h"],
    deps = ["@nearby_linux_platform//:linux_platform"],
)

cc_binary(
    name = "quickshare",
    srcs = ["main.cc"],
    deps = [
        ":send_session",
        "@nearby_linux_platform//:linux_platform",
    ],
)
