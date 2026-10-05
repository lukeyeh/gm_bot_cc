load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# What a GM is, who has said it when, and what that earns them. Knows nothing
# of Discord.
package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "gm",
    srcs = [
        "greeting.cc",
        "ledger.cc",
        "phrase.cc",
        "report.cc",
    ],
    hdrs = [
        "greeting.h",
        "ledger.h",
        "phrase.h",
        "report.h",
    ],
    deps = [
        "//sqlite:database",
        "@abseil-cpp",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":gm",
            "//sqlite:database",
            "//testing:main",
            "@abseil-cpp",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "greeting_test",
        "ledger_test",
        "phrase_test",
        "report_test",
    ]
]

# The ledger against a model, on arbitrary sequences of GMs. FuzzTest brings
# its own main, which is what understands --fuzz.
cc_test(
    name = "ledger_fuzz_test",
    size = "small",
    srcs = ["ledger_fuzz_test.cc"],
    deps = [
        ":gm",
        "@abseil-cpp",
        "@fuzztest",
        "@fuzztest//:main",
        "@gtest",
    ],
)
