load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# The bot itself: what joins Discord to the ledger of GMs.
package(default_visibility = [
    "//:__pkg__",
    "//perf:__pkg__",
])

cc_library(
    name = "bot",
    srcs = [
        "bot.cc",
        "config.cc",
    ],
    hdrs = [
        "bot.h",
        "config.h",
    ],
    deps = [
        "//async:status_macros",
        "//async:task",
        "//discord",
        "//gm",
        "//net:event_loop",
        "@abseil-cpp",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":bot",
            "//async:task",
            "//discord",
            "//gm",
            "//http",
            "//http:fake_client",
            "//net:event_loop",
            "//testing:main",
            "//websocket:fake_server",
            "@abseil-cpp",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "bot_test",
        "config_test",
    ]
]
