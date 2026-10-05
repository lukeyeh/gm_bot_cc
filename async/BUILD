load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Asynchronous functions. The only code that touches the C++ coroutine
# machinery; depends on nothing else in this repository.
package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "task",
    hdrs = ["task.h"],
)

cc_library(
    name = "awaitable",
    hdrs = ["awaitable.h"],
)

cc_library(
    name = "sequence",
    hdrs = ["sequence.h"],
)

cc_library(
    name = "status_macros",
    hdrs = ["status_macros.h"],
    deps = ["@abseil-cpp"],
)

cc_library(
    name = "task_scope",
    srcs = ["task_scope.cc"],
    hdrs = ["task_scope.h"],
    deps = [
        ":task",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "task_test",
    size = "small",
    srcs = ["task_test.cc"],
    deps = [
        ":task",
        ":task_scope",
        "//testing:main",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "awaitable_test",
    size = "small",
    srcs = ["awaitable_test.cc"],
    deps = [
        ":awaitable",
        ":task",
        ":task_scope",
        "//testing:main",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "task_scope_test",
    size = "small",
    srcs = ["task_scope_test.cc"],
    deps = [
        ":awaitable",
        ":task",
        ":task_scope",
        "//testing:main",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "status_macros_test",
    size = "small",
    srcs = ["status_macros_test.cc"],
    deps = [
        ":status_macros",
        ":task",
        ":task_scope",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "sequence_test",
    size = "small",
    srcs = ["sequence_test.cc"],
    deps = [
        ":awaitable",
        ":sequence",
        ":task",
        ":task_scope",
        "//testing:main",
        "@gbenchmark",
        "@gtest",
    ],
)
