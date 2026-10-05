load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "json",
    srcs = ["json.cc"],
    hdrs = ["json.h"],
    deps = ["@abseil-cpp"],
)

cc_test(
    name = "json_test",
    size = "small",
    srcs = ["json_test.cc"],
    deps = [
        ":json",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)
