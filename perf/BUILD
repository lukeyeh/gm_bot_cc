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
        "//async:status_macros",
        "//async:task",
        "//bot",
        "//discord",
        "//gm",
        "//http",
        "//net",
        "//net:event_loop",
        "//websocket",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "histogram_test",
    size = "small",
    srcs = ["histogram_test.cc"],
    deps = [
        ":histogram",
        "//testing:main",
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
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)
