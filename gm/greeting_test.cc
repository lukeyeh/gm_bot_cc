// What the bot says back, by example: to a GM, and to what was not one.

#include "gm/greeting.h"

#include <benchmark/benchmark.h>

#include <optional>
#include <string>
#include <vector>

#include "gm/ledger.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace gm {
namespace {

using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::Optional;

// A receipt for a counted GM that brought the streak to `streak`.
Receipt Counted(const int streak) {
  return Receipt{
      .counted = true,
      .new_record = false,
      .standing =
          Standing{
              .streak = streak,
              .best = streak,
          },
  };
}

// Someone who wrote something else in the GM channel is told what it cost
// them and what to say instead.
TEST(RebukeTest, SaysWhatWasLostAndWhatBelongs) {
  const std::vector<std::string> phrases = {
      "gm",
      "good morning",
      "morning",
  };

  EXPECT_EQ(Rebuke(12, "<@1>", phrases),
            "👎 <@1>, only GMs belong in this channel!\n"
            "**Your 12 day streak has been reset to 0.** 💔\n"
            "Say `gm`, `good morning` or `morning` to start a new one.");
}

// With no streak to lose there is no loss to report.
TEST(RebukeTest, LeavesOutTheLossWhenThereWasNone) {
  const std::vector<std::string> phrases = {
      "gm",
  };

  EXPECT_EQ(Rebuke(0, "<@1>", phrases),
            "👎 <@1>, only GMs belong in this channel!\n"
            "Say `gm` to start a new one.");
}

// A long list, or none at all, is not recited: the command that shows it is
// named instead.
TEST(RebukeTest, PointsToTheListWhenItIsLongOrEmpty) {
  const std::vector<std::string> many = {
      "a", "b", "c", "d", "e", "f",
  };

  EXPECT_THAT(Rebuke(0, "<@1>", many),
              testing::EndsWith("Use `/gmlist` to see what counts as a GM."));
  EXPECT_THAT(Rebuke(0, "<@1>", {}),
              testing::EndsWith("Use `/gmlist` to see what counts as a GM."));
}

// Most GMs get the emoji and nothing more, so the channel stays quiet.
TEST(AnnouncementTest, OrdinaryDaysNeedNoMessage) {
  for (const int streak : {2, 3, 4, 5, 6, 8, 9, 11}) {
    EXPECT_THAT(Announcement(Counted(streak), "<@1>"), Eq(std::nullopt))
        << streak;
  }
}

TEST(AnnouncementTest, WelcomesANewStreak) {
  EXPECT_THAT(Announcement(Counted(1), "<@1>"),
              Optional(Eq("Good morning <@1>! Your streak has started! ☀️")));
}

// A repeat is answered, so people know it did not count twice.
TEST(AnnouncementTest, SaysWhenAGmWasAlreadyCounted) {
  Receipt repeat = Counted(1);
  repeat.counted = false;

  EXPECT_THAT(Announcement(repeat, "<@1>"),
              Optional(HasSubstr("you already said GM today! Your current "
                                 "streak is **1 day**")));
}

TEST(AnnouncementTest, CelebratesANewRecord) {
  Receipt record = Counted(4);
  record.new_record = true;

  EXPECT_THAT(Announcement(record, "<@1>"),
              Optional(HasSubstr("New personal record!** <@1> is on a **4 day "
                                 "streak!")));
}

TEST(AnnouncementTest, CelebratesWholeWeeks) {
  EXPECT_THAT(Announcement(Counted(7), "<@1>"),
              Optional(HasSubstr("saying GM for 1 week!")));
  EXPECT_THAT(Announcement(Counted(21), "<@1>"),
              Optional(HasSubstr("**21 days!** <@1> has been saying GM for 3 "
                                 "weeks!")));
}

TEST(AnnouncementTest, CelebratesRoundNumbers) {
  for (const int streak : {10, 25, 50, 100}) {
    EXPECT_THAT(Announcement(Counted(streak), "<@1>"),
                Optional(HasSubstr("MILESTONE!")))
        << streak;
  }
}

// When a day is special twice over, the record wins: it is the rarer event.
TEST(AnnouncementTest, ARecordOutranksAMilestone) {
  Receipt record = Counted(7);
  record.new_record = true;

  EXPECT_THAT(Announcement(record, "<@1>"),
              Optional(HasSubstr("New personal record!")));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //gm:greeting_test -- --benchmark_filter=all
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
//   ----------------------------------------------------------
//   Benchmark                Time             CPU   Iterations
//   ----------------------------------------------------------
//   BM_Announcement       48.5 ns         48.5 ns      8710447
// End of results.
// clang-format on

void BM_Announcement(benchmark::State& state) {
  const Receipt milestone = Counted(21);
  const Receipt ordinary = Counted(4);
  for (auto _ : state) {
    benchmark::DoNotOptimize(Announcement(milestone, "<@400000000000000004>"));
    benchmark::DoNotOptimize(Announcement(ordinary, "<@400000000000000004>"));
  }
}
BENCHMARK(BM_Announcement);

}  // namespace
}  // namespace gm
