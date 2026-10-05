load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# A Discord bot's view of Discord. Users of this package include client.h and
# model.h; the rest is how the client is built.
package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "discord",
    srcs = [
        "client.cc",
        "gateway.cc",
        "gateway.h",
        "rest.cc",
        "rest.h",
        "wire.cc",
        "wire.h",
    ],
    hdrs = [
        "client.h",
        "model.h",
    ],
    deps = [
        "//async:sequence",
        "//async:status_macros",
        "//async:task",
        "//http",
        "//json",
        "//net:event_loop",
        "//websocket",
        "@abseil-cpp",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [
            name + ".cc",
            "gateway.h",
            "rest.h",
            "wire.h",
        ],
        deps = [
            ":discord",
            "//async:task",
            "//http",
            "//http:fake_client",
            "//json",
            "//net:event_loop",
            "//testing:main",
            "//websocket",
            "//websocket:fake_server",
            "@abseil-cpp",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "client_test",
        "gateway_test",
        "model_test",
        "rest_test",
        "wire_test",
    ]
]
