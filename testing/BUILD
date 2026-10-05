load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//:__subpackages__"])

# Link into every cc_test in place of @googletest//:gtest_main. Provides a
# main that runs the file's tests, or its benchmarks when asked.
cc_library(
    name = "main",
    testonly = True,
    srcs = ["main.cc"],
    deps = [
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)
