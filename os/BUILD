load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# The only code that talks to the kernel. Everything else reaches it through
# //net.
package(default_visibility = ["//net:__pkg__"])

cc_library(
    name = "socket",
    srcs = ["socket.cc"],
    hdrs = ["socket.h"],
    deps = [
        "@abseil-cpp",
    ],
)

cc_library(
    name = "io",
    srcs = [
        "epoll_backend.cc",
        "epoll_backend.h",
        "io.cc",
        "io_uring_backend.cc",
        "io_uring_backend.h",
    ],
    hdrs = ["io.h"],
    deps = [
        ":socket",
        "//async:awaitable",
        "@abseil-cpp",
        "@liburing",
    ],
)

cc_test(
    name = "socket_test",
    size = "small",
    srcs = ["socket_test.cc"],
    deps = [
        ":socket",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "io_test",
    size = "small",
    srcs = ["io_test.cc"],
    deps = [
        ":io",
        ":socket",
        "//async:task",
        "//async:task_scope",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "io_uring_backend_test",
    size = "small",
    srcs = [
        "io_uring_backend.h",
        "io_uring_backend_test.cc",
    ],
    deps = [
        ":io",
        ":socket",
        "//async:task",
        "//async:task_scope",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "epoll_backend_test",
    size = "small",
    srcs = [
        "epoll_backend.h",
        "epoll_backend_test.cc",
    ],
    deps = [
        ":io",
        ":socket",
        "//async:task",
        "//async:task_scope",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)
