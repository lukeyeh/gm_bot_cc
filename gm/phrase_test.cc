// Which messages count as a GM, and which phrases can be made to count, by
// example.

#include "gm/phrase.h"

#include <benchmark/benchmark.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using gm::CanonicalPhrase;
using gm::Hours;
using gm::Phrase;
using gm::Reading;
using testing::HasSubstr;

// Phrases with these texts that count all day.
std::vector<Phrase> AllDay(std::span<const std::string_view> texts) {
  std::vector<Phrase> phrases;
  for (const std::string_view text : texts) {
    phrases.push_back(Phrase{
        .text = std::string(text),
    });
  }
  return phrases;
}

// The phrases every ledger starts with.
std::vector<Phrase> Defaults() { return AllDay(gm::kDefaultPhrases); }

// Whether `message` is a GM given `phrases`, at an hour that does not matter
// because they count all day.
bool IsGm(std::string_view message, std::span<const Phrase> phrases) {
  return gm::Read(message, phrases, 0).kind == Reading::Kind::kGm;
}

// The hours from `from` until `until`.
Hours Between(int from, int until) {
  return Hours::Between(from, until).value();
}

TEST(ReadTest, AcceptsTheUsualPhrasesInAnyCase) {
  const std::vector<Phrase> phrases = Defaults();
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
TEST(ReadTest, AcceptsAPhraseAtEitherEndOfALongerMessage) {
  const std::vector<Phrase> phrases = Defaults();
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
TEST(ReadTest, RejectsEverythingElse) {
  const std::vector<Phrase> phrases = Defaults();
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
TEST(ReadTest, GoesByTheListItIsGiven) {
  const std::vector<Phrase> phrases = {
      Phrase{
          .text = "buenos días",
      },
      Phrase{
          .text = "yo",
      },
  };

  EXPECT_TRUE(IsGm("Yo!", phrases));
  EXPECT_TRUE(IsGm("buenos días a todos", phrases));
  EXPECT_FALSE(IsGm("gm", phrases));
  EXPECT_FALSE(IsGm("yoghurt", phrases));

  // With no phrases, nothing is a GM.
  EXPECT_FALSE(IsGm("gm", {}));
}

// A phrase can be limited to some hours of the day. Said outside them it is
// still recognised, as out of hours, which is not the same as saying
// something else.
TEST(ReadTest, APhraseCountsOnlyDuringItsHours) {
  const std::vector<Phrase> phrases = {
      Phrase{
          .text = "good morning",
          .hours = Between(5, 12),
      },
  };

  const Reading at_nine = gm::Read("Good morning!", phrases, 9);
  EXPECT_EQ(at_nine.kind, Reading::Kind::kGm);
  EXPECT_EQ(at_nine.phrase, &phrases[0]);

  const Reading at_three = gm::Read("Good morning!", phrases, 15);
  EXPECT_EQ(at_three.kind, Reading::Kind::kOutOfHours);
  EXPECT_EQ(at_three.phrase, &phrases[0]);

  const Reading other = gm::Read("hello", phrases, 9);
  EXPECT_EQ(other.kind, Reading::Kind::kOther);
  EXPECT_EQ(other.phrase, nullptr);
}

// A message can say more than one phrase: "gm" and "gm gm" both open "gm
// gm". It is a GM if any of them counts now, whatever order they are in.
TEST(ReadTest, AMessageSayingSeveralPhrasesNeedsOnlyOneInHours) {
  const std::vector<Phrase> phrases = {
      Phrase{
          .text = "gm",
          .hours = Between(5, 12),
      },
      Phrase{
          .text = "gm gm",
      },
  };

  const Reading evening = gm::Read("gm gm", phrases, 20);
  EXPECT_EQ(evening.kind, Reading::Kind::kGm);
  EXPECT_EQ(evening.phrase, &phrases[1]);

  // When none counts, the first is the one reported.
  const std::vector<Phrase> both_limited = {
      Phrase{
          .text = "gm",
          .hours = Between(5, 12),
      },
      Phrase{
          .text = "gm gm",
          .hours = Between(6, 9),
      },
  };
  const Reading none = gm::Read("gm gm", both_limited, 20);
  EXPECT_EQ(none.kind, Reading::Kind::kOutOfHours);
  EXPECT_EQ(none.phrase, &both_limited[0]);
}

// Hours start with the first and stop before the second.
TEST(HoursTest, RunFromOneHourUntilAnother) {
  const Hours mornings = Between(5, 12);

  EXPECT_FALSE(mornings.Contains(4));
  EXPECT_TRUE(mornings.Contains(5));
  EXPECT_TRUE(mornings.Contains(11));
  EXPECT_FALSE(mornings.Contains(12));
  EXPECT_FALSE(mornings.all_day());
  EXPECT_EQ(mornings.Describe(), "5:00 to 12:00");
}

// When the second hour is not later than the first, they run past midnight.
TEST(HoursTest, CanRunPastMidnight) {
  const Hours nights = Between(22, 2);

  EXPECT_TRUE(nights.Contains(22));
  EXPECT_TRUE(nights.Contains(23));
  EXPECT_TRUE(nights.Contains(0));
  EXPECT_TRUE(nights.Contains(1));
  EXPECT_FALSE(nights.Contains(2));
  EXPECT_FALSE(nights.Contains(21));

  // Midnight can be written as 24 at the end of the day.
  EXPECT_EQ(Between(18, 24), Between(18, 0));
  EXPECT_TRUE(Between(18, 24).Contains(23));
  EXPECT_FALSE(Between(18, 24).Contains(0));
}

// By default, and from any hour round to itself, hours are the whole day.
// However that is written it is the same thing.
TEST(HoursTest, AreAllDayUnlessLimited) {
  EXPECT_TRUE(Hours().all_day());
  EXPECT_EQ(Between(7, 7), Hours());
  EXPECT_EQ(Between(0, 24), Hours());
  EXPECT_EQ(Hours().Describe(), "any time");

  for (int hour = 0; hour < 24; ++hour) {
    EXPECT_TRUE(Hours().Contains(hour)) << hour;
  }
}

// People write hours as "5-12", or "anytime".
TEST(HoursTest, AreReadAsPeopleWriteThem) {
  EXPECT_THAT(Hours::Parse("5-12"), IsOkAndHolds(Between(5, 12)));
  EXPECT_THAT(Hours::Parse(" 22-2 "), IsOkAndHolds(Between(22, 2)));
  EXPECT_THAT(Hours::Parse("anytime"), IsOkAndHolds(Hours()));
  EXPECT_THAT(Hours::Parse("Anytime"), IsOkAndHolds(Hours()));
}

// What is not hours is refused in words fit to show whoever wrote it.
TEST(HoursTest, RefuseWhatAreNotHours) {
  for (const char* const text :
       {"", "5", "five to twelve", "5-", "-5", "5-12-3", "5:00-12:00"}) {
    EXPECT_THAT(Hours::Parse(text), StatusIs(absl::StatusCode::kInvalidArgument,
                                             HasSubstr("like `5-12`")))
        << text;
  }
  for (const char* const text : {"5-25", "24-3", "99-100"}) {
    EXPECT_THAT(Hours::Parse(text), StatusIs(absl::StatusCode::kInvalidArgument,
                                             HasSubstr("from 0 to 23")))
        << text;
  }
  EXPECT_THAT(Hours::Between(-1, 5),
              StatusIs(absl::StatusCode::kInvalidArgument));
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
    const std::vector<Phrase> phrases = {
        Phrase{
            .text = *phrase,
        },
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
//   Date      2026-10-06
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
//   BM_Read                  60.8 ns         60.8 ns      6901487
//   BM_ParseHours            30.9 ns         30.9 ns     13486315
//   BM_CanonicalPhrase       18.4 ns         18.4 ns     22873327
// End of results.
// clang-format on

// The check made on every message in a GM channel: one that is a GM and one
// that is not.
void BM_Read(benchmark::State& state) {
  const std::vector<Phrase> phrases = Defaults();
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        gm::Read("GM everyone, hope the coffee is strong", phrases, 9));
    benchmark::DoNotOptimize(gm::Read(
        "did anyone see the game last night? what a finish", phrases, 9));
  }
}
BENCHMARK(BM_Read);

void BM_ParseHours(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(Hours::Parse("5-12"));
}
BENCHMARK(BM_ParseHours);

void BM_CanonicalPhrase(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(CanonicalPhrase("  Good Morning  "));
  }
}
BENCHMARK(BM_CanonicalPhrase);

}  // namespace
