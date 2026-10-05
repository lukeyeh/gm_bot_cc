// Which messages count as a GM, and which phrases can be made to count, by
// example.

#include "gm/phrase.h"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using gm::CanonicalPhrase;
using gm::IsGm;
using testing::HasSubstr;

// The phrases every ledger starts with.
std::vector<std::string> Defaults() {
  return std::vector<std::string>(gm::kDefaultPhrases.begin(),
                                  gm::kDefaultPhrases.end());
}

TEST(IsGmTest, AcceptsTheUsualPhrasesInAnyCase) {
  const std::vector<std::string> phrases = Defaults();
  for (const char* const message : {
           "gm",
           "GM",
           "Gm!",
           "good morning",
           "Good Morning!",
           "morning",
           "  gm  ",
       }) {
    EXPECT_TRUE(IsGm(message, phrases)) << message;
  }
}

// People rarely say only "gm".
TEST(IsGmTest, AcceptsAPhraseAtEitherEndOfALongerMessage) {
  const std::vector<std::string> phrases = Defaults();
  for (const char* const message : {
           "gm everyone",
           "gm, all ☀️",
           "GM!! what a day",
           "ok ok, good morning",
           "rise and shine. gm",
       }) {
    EXPECT_TRUE(IsGm(message, phrases)) << message;
  }
}

// The phrase has to be a word, and has to open or close the message.
TEST(IsGmTest, RejectsEverythingElse) {
  const std::vector<std::string> phrases = Defaults();
  for (const char* const message : {
           "",
           "hello",
           "gmail is down",
           "program",
           "did anyone say gm yet today",
           "good night",
       }) {
    EXPECT_FALSE(IsGm(message, phrases)) << message;
  }
}

// What counts is whatever the list says: a server can have its own.
TEST(IsGmTest, GoesByTheListItIsGiven) {
  const std::vector<std::string> phrases = {
      "buenos días",
      "yo",
  };

  EXPECT_TRUE(IsGm("Yo!", phrases));
  EXPECT_TRUE(IsGm("buenos días a todos", phrases));
  EXPECT_FALSE(IsGm("gm", phrases));
  EXPECT_FALSE(IsGm("yoghurt", phrases));

  // With no phrases, nothing is a GM.
  EXPECT_FALSE(IsGm("gm", {}));
}

// Phrases are kept trimmed and in lower case, so that the same phrase typed
// two ways is one phrase.
TEST(CanonicalPhraseTest, TrimsAndLowers) {
  EXPECT_THAT(CanonicalPhrase("  Good Morning "), IsOkAndHolds("good morning"));
  EXPECT_THAT(CanonicalPhrase("GM!"), IsOkAndHolds("gm!"));
  EXPECT_THAT(CanonicalPhrase("buenos días"), IsOkAndHolds("buenos días"));
}

// What cannot be a phrase, each with a reason fit to show the person who
// proposed it.
TEST(CanonicalPhraseTest, RejectsWhatCannotBeAPhrase) {
  EXPECT_THAT(CanonicalPhrase(""), StatusIs(absl::StatusCode::kInvalidArgument,
                                            HasSubstr("cannot be empty")));
  EXPECT_THAT(CanonicalPhrase("   "),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(CanonicalPhrase(std::string(51, 'g')),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("longer than 50")));
  EXPECT_THAT(CanonicalPhrase("good\nmorning"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(
      CanonicalPhrase("`gm`"),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("backticks")));

  // Exactly as long as allowed is allowed.
  EXPECT_THAT(CanonicalPhrase(std::string(50, 'g')),
              IsOkAndHolds(std::string(50, 'g')));
}

// A phrase, once accepted, is said by any message consisting of it. This is
// what makes adding a phrase mean something.
TEST(CanonicalPhraseTest, AnAcceptedPhraseIsSaidByItself) {
  for (const char* const proposed : {"Yo", " GM! ", "buenos días", "¡hola!"}) {
    const absl::StatusOr<std::string> phrase = CanonicalPhrase(proposed);
    ABSL_ASSERT_OK(phrase);
    const std::vector<std::string> phrases = {
        *phrase,
    };

    EXPECT_TRUE(IsGm(proposed, phrases)) << proposed;
  }
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //gm:phrase_test -- --benchmark_filter=all
//
// clang-format off
// Results. Written by perf/record_results.py; do not edit by hand.
//
//   Date      2026-10-05
//   CPU       Intel(R) Core(TM) i7-9700 CPU @ 3.00GHz, 8 cores, L3 12 MiB (1 instance)
//   Memory    31 GB
//   Disk      Samsung SSD 990 EVO Plus 4TB (ext4)
//   System    Linux 7.0.0-38-generic, CPU governor: powersave
//   Compiler  clang version 21.1.8
//   Build     bazel -c opt (-O2), C++20, no exceptions
//   I/O       io_uring, except where a benchmark's name says epoll
//
//   -------------------------------------------------------------
//   Benchmark                   Time             CPU   Iterations
//   -------------------------------------------------------------
//   BM_IsGm                  57.1 ns         57.0 ns      7158317
//   BM_CanonicalPhrase       22.8 ns         22.0 ns     19303447
// End of results.
// clang-format on

// The check made on every message in a GM channel.
void BM_IsGm(benchmark::State& state) {
  const std::vector<std::string> phrases = Defaults();
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        IsGm("GM everyone, hope the coffee is strong", phrases));
    benchmark::DoNotOptimize(
        IsGm("did anyone see the game last night? what a finish", phrases));
  }
}
BENCHMARK(BM_IsGm);

void BM_CanonicalPhrase(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(CanonicalPhrase("  Good Morning  "));
  }
}
BENCHMARK(BM_CanonicalPhrase);

}  // namespace
