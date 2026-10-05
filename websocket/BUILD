load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "websocket",
    srcs = ["websocket.cc"],
    hdrs = ["websocket.h"],
    deps = [
        "//async:status_macros",
        "//async:task",
        "//http",
        "//net",
        "@abseil-cpp",
        "@openssl",
    ],
)

# A scripted server for tests of code that talks over a WebSocket.
cc_library(
    name = "fake_server",
    testonly = True,
    hdrs = ["fake_server.h"],
    deps = [
        ":websocket",
        "//async:task",
        "@abseil-cpp",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":fake_server",
            ":websocket",
            "//async:task",
            "//net",
            "//net:event_loop",
            "//testing:main",
            "@abseil-cpp",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "fake_server_test",
        "websocket_test",
    ]
]
