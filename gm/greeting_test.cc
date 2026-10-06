// What the bot says back, by example: to a GM, and to what was not one.

#include "gm/greeting.h"

#include <benchmark/benchmark.h>

#include <initializer_list>
#include <optional>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "gm/ledger.h"
#include "gm/phrase.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace gm {
namespace {

using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::Optional;

// A receipt for a counted GM that brought a first streak to `streak`, which
// is therefore also the member's best.
Receipt Counted(const int streak) {
  return Receipt{
      .counted = true,
      .new_record = false,
      .standing =
          Standing{
              .streak = streak,
              .best = streak,
          },
      .best_before = streak - 1,
  };
}

// The same for someone who has done better before, so that this streak
// raises no best and wins no badge.
Receipt CountedBelowBest(const int streak) {
  Receipt receipt = Counted(streak);
  receipt.standing.best = 400;
  receipt.best_before = 400;
  return receipt;
}

// Phrases with these texts that count all day.
std::vector<Phrase> AllDay(std::initializer_list<const char*> texts) {
  std::vector<Phrase> phrases;
  for (const char* const text : texts) {
    phrases.push_back(Phrase{
        .text = text,
    });
  }
  return phrases;
}

// Someone who wrote something else in the GM channel is told what it cost
// them and what to say instead.
TEST(RebukeTest, SaysWhatWasLostAndWhatBelongs) {
  const std::vector<Phrase> phrases = AllDay({"gm", "good morning", "morning"});

  EXPECT_EQ(Rebuke(12, "<@1>", phrases),
            "👎 <@1>, only GMs belong in this channel!\n"
            "**Your 12 day streak has been reset to 0.** 💔\n"
            "Say `gm`, `good morning` or `morning` to start a new one.");
}

// With no streak to lose there is no loss to report.
TEST(RebukeTest, LeavesOutTheLossWhenThereWasNone) {
  const std::vector<Phrase> phrases = AllDay({"gm"});

  EXPECT_EQ(Rebuke(0, "<@1>", phrases),
            "👎 <@1>, only GMs belong in this channel!\n"
            "Say `gm` to start a new one.");
}

// A long list, or none at all, is not recited: the command that shows it is
// named instead.
TEST(RebukeTest, PointsToTheListWhenItIsLongOrEmpty) {
  const std::vector<Phrase> many = AllDay({"a", "b", "c", "d", "e", "f"});

  EXPECT_THAT(Rebuke(0, "<@1>", many),
              testing::EndsWith("Use `/gmlist` to see what counts as a GM."));
  EXPECT_THAT(Rebuke(0, "<@1>", {}),
              testing::EndsWith("Use `/gmlist` to see what counts as a GM."));
}

// A phrase said outside its hours is not counted, and its author is told
// when it would be and what the bot's clock says.
TEST(OutOfHoursNoticeTest, SaysWhenThePhraseCounts) {
  const absl::StatusOr<gm::Hours> mornings = gm::Hours::Between(5, 12);
  ABSL_ASSERT_OK(mornings);
  const Phrase phrase = {
      .text = "good morning",
      .hours = *mornings,
  };

  EXPECT_EQ(gm::OutOfHoursNotice(phrase, "<@1>",
                                 absl::CivilMinute(2026, 10, 5, 14, 5),
                                 "America/New_York"),
            "⏰ <@1>, `good morning` only counts from **5:00 to 12:00**. It "
            "is now 14:05 (America/New_York).");
}

// Most GMs get the emoji and nothing more, so the channel stays quiet.
TEST(AnnouncementTest, OrdinaryDaysNeedNoMessage) {
  for (const int streak : {2, 4, 5, 6, 8, 9, 11}) {
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
  repeat.best_before = 1;

  EXPECT_THAT(Announcement(repeat, "<@1>"),
              Optional(HasSubstr("you already said GM today! Your current "
                                 "streak is **1 day**")));
}

// The GM that takes someone's best streak to a badge's length wins it.
TEST(AnnouncementTest, AnnouncesABadgeWon) {
  EXPECT_THAT(Announcement(Counted(3), "<@1>"),
              Optional(Eq("🌱 **NEW BADGE UNLOCKED!** <@1> earned the "
                          "**Sprout** badge with a **3 day streak!** 🌱")));
  EXPECT_THAT(Announcement(Counted(100), "<@1>"),
              Optional(HasSubstr("earned the **Centurion** badge")));
}

// A badge is won once. A later streak passing the same length raises no
// best, so it is an ordinary day, or whatever else that length makes it.
TEST(AnnouncementTest, DoesNotAnnounceABadgeAlreadyHeld) {
  EXPECT_THAT(Announcement(CountedBelowBest(3), "<@1>"), Eq(std::nullopt));
  EXPECT_THAT(Announcement(CountedBelowBest(7), "<@1>"),
              Optional(HasSubstr("saying GM for 1 week!")));
}

TEST(AnnouncementTest, CelebratesANewRecord) {
  Receipt record = Counted(4);
  record.new_record = true;

  EXPECT_THAT(Announcement(record, "<@1>"),
              Optional(HasSubstr("New personal record!** <@1> is on a **4 day "
                                 "streak!")));
}

// Whole weeks are marked, where a badge does not mark them already.
TEST(AnnouncementTest, CelebratesWholeWeeks) {
  EXPECT_THAT(Announcement(Counted(28), "<@1>"),
              Optional(HasSubstr("**28 days!** <@1> has been saying GM for 4 "
                                 "weeks!")));
  EXPECT_THAT(Announcement(CountedBelowBest(21), "<@1>"),
              Optional(HasSubstr("saying GM for 3 weeks!")));
}

// So are round numbers, likewise.
TEST(AnnouncementTest, CelebratesRoundNumbers) {
  for (const int streak : {10, 25}) {
    EXPECT_THAT(Announcement(Counted(streak), "<@1>"),
                Optional(HasSubstr("MILESTONE!")))
        << streak;
  }
  for (const int streak : {50, 100}) {
    EXPECT_THAT(Announcement(CountedBelowBest(streak), "<@1>"),
                Optional(HasSubstr("MILESTONE!")))
        << streak;
  }
}

// When a day is special more than one way, a badge comes first, being won
// only once, and then a record.
TEST(AnnouncementTest, ABadgeOutranksARecordWhichOutranksAMilestone) {
  Receipt badge_and_record = Counted(7);
  badge_and_record.new_record = true;
  EXPECT_THAT(Announcement(badge_and_record, "<@1>"),
              Optional(HasSubstr("NEW BADGE UNLOCKED!")));

  Receipt record_and_week = Counted(28);
  record_and_week.new_record = true;
  EXPECT_THAT(Announcement(record_and_week, "<@1>"),
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
//   Date      2026-10-06
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
//   BM_Announcement       59.5 ns         59.4 ns      7071882
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
