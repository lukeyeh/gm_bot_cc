load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Nothing here talks to the kernel; that is //os.

# Anything asynchronous needs the loop that runs it.
cc_library(
    name = "event_loop",
    srcs = ["event_loop.cc"],
    hdrs = ["event_loop.h"],
    visibility = ["//:__subpackages__"],
    deps = [
        "//async:task",
        "//async:task_scope",
        "//os:io",
        "@abseil-cpp",
    ],
)

# Byte streams to other machines: TCP, TLS, and what protocols need to read
# from them. Only the protocol packages do network I/O directly.
cc_library(
    name = "net",
    srcs = [
        "reader.cc",
        "stream.cc",
        "tls.cc",
        "url.cc",
    ],
    hdrs = [
        "reader.h",
        "stream.h",
        "tls.h",
        "url.h",
    ],
    visibility = [
        "//http:__pkg__",
        "//perf:__pkg__",
        "//websocket:__pkg__",
    ],
    deps = [
        "//async:status_macros",
        "//async:task",
        "//os:io",
        "//os:socket",
        "@abseil-cpp",
        "@openssl",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":event_loop",
            ":net",
            "//async:task",
            "//testing:main",
            "@abseil-cpp",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "event_loop_test",
        "reader_test",
        "stream_test",
        "tls_test",
        "url_test",
    ]
]
