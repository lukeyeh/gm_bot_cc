// The main function of every test in this repository.
//
// A _test.cc file holds two things about the code it covers: tests, which say
// whether it works, and benchmarks, which say what it costs. One binary runs
// either:
//
//   bazel test //gm:ledger_test
//       Runs the tests.
//
//   bazel run -c opt //gm:ledger_test -- --benchmark_filter=all
//       Runs the benchmarks instead. Any flag starting with --benchmark
//       selects this mode; --benchmark_filter takes a regular expression to
//       choose which ones.
//
// Either way, flags defined by the code under test can be given too. The one
// that matters most is --io_backend=epoll, which runs the same tests or
// benchmarks on the other I/O backend:
//
//   bazel test --config=epoll //...

#include <benchmark/benchmark.h>

#include <iostream>
#include <string_view>
#include <vector>

#include "absl/flags/parse.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

int main(int argc, char** argv) {
  bool benchmarks_requested = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]).starts_with("--benchmark")) {
      benchmarks_requested = true;
    }
  }

  // Each library takes its own flags out of the command line. What is left
  // goes to the flags the code under test defines, such as --io_backend.
  testing::InitGoogleMock(&argc, argv);
  if (benchmarks_requested) benchmark::Initialize(&argc, argv);
  std::vector<char*> positional;
  std::vector<absl::UnrecognizedFlag> unrecognized;
  absl::ParseAbseilFlagsOnly(argc, argv, positional, unrecognized);
  // A flag this test does not define is skipped rather than refused, so that
  // one command line can be given to every test: --io_backend means nothing
  // to a test that does no I/O.
  for (const absl::UnrecognizedFlag& flag : unrecognized) {
    std::cerr << "note: ignoring --" << flag.flag_name
              << ", which nothing in this test defines\n";
  }

  if (!benchmarks_requested) return RUN_ALL_TESTS();
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
