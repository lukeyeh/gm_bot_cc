load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "database",
    srcs = ["database.cc"],
    hdrs = ["database.h"],
    deps = [
        "@abseil-cpp",
        "@sqlite",
    ],
)

cc_test(
    name = "database_test",
    size = "small",
    srcs = ["database_test.cc"],
    deps = [
        ":database",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)
