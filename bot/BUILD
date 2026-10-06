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
        "//gm",
        "@abseil-cpp",
        "@bedrock//:async",
        "@bedrock//:discord",
        "@bedrock//:net",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":bot",
            "//gm",
            "@abseil-cpp",
            "@bedrock//:async",
            "@bedrock//:discord",
            "@bedrock//:http",
            "@bedrock//:net",
            "@bedrock//:testing_main",
            "@bedrock//:websocket",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "bot_test",
        "config_test",
    ]
]
