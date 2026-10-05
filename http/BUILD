load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# The client side of HTTP/1.1, which is all a bot needs: it calls a web API
# and opens WebSockets, and serves nothing.
package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "http",
    srcs = [
        "client.cc",
        "head.cc",
    ],
    hdrs = [
        "client.h",
        "head.h",
    ],
    deps = [
        "//async:status_macros",
        "//async:task",
        "//net",
        "@abseil-cpp",
    ],
)

# A Client for tests of code that makes HTTP requests.
cc_library(
    name = "fake_client",
    testonly = True,
    hdrs = ["fake_client.h"],
    deps = [
        ":http",
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
            ":fake_client",
            ":http",
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
        "client_test",
        "fake_client_test",
        "head_test",
    ]
]
