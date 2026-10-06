load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Load tests: a stand-in for Discord that floods a client with events over a
# real socket, and the program that runs it. Nothing else depends on this.
package(default_visibility = ["//visibility:private"])

cc_binary(
    name = "bench",
    testonly = True,
    srcs = ["bench.cc"],
    deps = [
        ":load",
        "@abseil-cpp",
    ],
)

cc_library(
    name = "histogram",
    srcs = ["histogram.cc"],
    hdrs = ["histogram.h"],
)

cc_library(
    name = "load",
    testonly = True,
    srcs = ["load.cc"],
    hdrs = ["load.h"],
    deps = [
        ":histogram",
        "//bot",
        "//gm",
        "@abseil-cpp",
        "@bedrock//:async",
        "@bedrock//:discord",
        "@bedrock//:http",
        "@bedrock//:net",
        "@bedrock//:websocket",
    ],
)

cc_test(
    name = "histogram_test",
    size = "small",
    srcs = ["histogram_test.cc"],
    deps = [
        ":histogram",
        "@bedrock//:testing_main",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "load_test",
    size = "small",
    srcs = ["load_test.cc"],
    deps = [
        ":load",
        "@abseil-cpp",
        "@bedrock//:testing_main",
        "@gbenchmark",
        "@gtest",
    ],
)
