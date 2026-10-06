// What the bot says when asked about streaks, by example.

#include "gm/report.h"

#include <benchmark/benchmark.h>

#include <string>
#include <utility>
#include <vector>

#include "gm/ledger.h"
#include "gm/phrase.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

gm::Standing Standing(std::string name, int streak, int best) {
  return gm::Standing{
      .member =
          gm::Member{
              .id = 1,
              .name = std::move(name),
          },
      .streak = streak,
      .best = best,
  };
}

// A phrase that counts from hour `from` until hour `until`: by default, all
// day.
gm::Phrase Phrase(std::string text, int from = 0, int until = 0) {
  return gm::Phrase{
      .text = std::move(text),
      .hours = gm::Hours::Between(from, until).value(),
  };
}

// The whole of a typical leaderboard: medals for the first three places,
// numbers after, and a best streak shown only where it beats the live one.
TEST(LeaderboardReportTest, RanksWithMedalsThenNumbers) {
  const std::vector<gm::Standing> board = {
      Standing("ada", 12, 12),
      Standing("luke", 5, 30),
      Standing("grace", 2, 2),
      Standing("alan", 1, 1),
  };

  EXPECT_EQ(gm::LeaderboardReport(board),
            "🌅 **GM Streak Leaderboard**\n"
            "\n"
            "🥇 **ada** — 12 days\n"
            "🥈 **luke** — 5 days (best: 30)\n"
            "🥉 **grace** — 2 days\n"
            "**4.** **alan** — 1 day");
}

// Equal streaks share a place, and the next place follows on directly rather
// than skipping.
TEST(LeaderboardReportTest, EqualStreaksShareAPlace) {
  const std::vector<gm::Standing> board = {
      Standing("ada", 3, 3),
      Standing("luke", 3, 3),
      Standing("grace", 1, 1),
  };

  EXPECT_EQ(gm::LeaderboardReport(board),
            "🌅 **GM Streak Leaderboard**\n"
            "\n"
            "🥇 **ada** — 3 days\n"
            "🥇 **luke** — 3 days\n"
            "🥈 **grace** — 1 day");
}

// Someone whose streak has lapsed is still on the board, with what they once
// managed.
TEST(LeaderboardReportTest, ShowsLapsedStreaksAsZero) {
  const std::vector<gm::Standing> board = {
      Standing("luke", 0, 7),
  };

  EXPECT_EQ(gm::LeaderboardReport(board),
            "🌅 **GM Streak Leaderboard**\n"
            "\n"
            "🥇 **luke** — 0 days (best: 7)");
}

TEST(LeaderboardReportTest, InvitesTheFirstGm) {
  EXPECT_EQ(gm::LeaderboardReport({}),
            "🌅 **GM Streak Leaderboard**\n"
            "\n"
            "No one has said GM yet. Be the first!");
}

// A name is shown as its owner wrote it, whatever Discord would otherwise
// make of the characters in it.
TEST(LeaderboardReportTest, ShowsNamesLiterally) {
  const std::vector<gm::Standing> board = {
      Standing("_luke_ **the** `early`", 1, 1),
  };

  EXPECT_EQ(gm::LeaderboardReport(board),
            "🌅 **GM Streak Leaderboard**\n"
            "\n"
            "🥇 **\\_luke\\_ \\*\\*the\\*\\* \\`early\\`** — 1 day");
}

TEST(PhraseListReportTest, ListsThePhrases) {
  const std::vector<gm::Phrase> phrases = {
      Phrase("gm"),
      Phrase("good morning"),
  };

  EXPECT_EQ(gm::PhraseListReport(phrases),
            "✅ **GM phrases**\n"
            "\n"
            "• `gm`\n"
            "• `good morning`\n"
            "\n"
            "Capitalisation does not matter.");
}

// A phrase limited to some hours is listed with them.
TEST(PhraseListReportTest, ShowsTheHoursOfALimitedPhrase) {
  const std::vector<gm::Phrase> phrases = {
      Phrase("gm"),
      Phrase("good morning", 5, 12),
      Phrase("gn", 22, 2),
  };

  EXPECT_EQ(gm::PhraseListReport(phrases),
            "✅ **GM phrases**\n"
            "\n"
            "• `gm`\n"
            "• `good morning` — ⏰ 5:00 to 12:00\n"
            "• `gn` — ⏰ 22:00 to 2:00\n"
            "\n"
            "Capitalisation does not matter. Hours are on a 24-hour clock.");
}

// An empty list is worth explaining, since it means every message in the GM
// channel costs a streak.
TEST(PhraseListReportTest, ExplainsAnEmptyList) {
  EXPECT_EQ(gm::PhraseListReport({}),
            "✅ **GM phrases**\n"
            "\n"
            "There are none, so nothing counts as a GM. An admin can add one "
            "with `/gmadd`.");
}

// Adding and removing each say what happened, including when nothing did.
TEST(PhraseChangeReportTest, SaysWhatHappened) {
  EXPECT_EQ(gm::PhraseAddedReport(Phrase("yo"), true),
            "✅ `yo` now counts as a GM.");
  EXPECT_EQ(gm::PhraseAddedReport(Phrase("yo", 5, 12), true),
            "✅ `yo` now counts as a GM from 5:00 to 12:00.");
  EXPECT_EQ(gm::PhraseAddedReport(Phrase("gm"), false),
            "`gm` already counts as a GM.");
  EXPECT_EQ(gm::PhraseRemovedReport("yo", true),
            "✅ `yo` no longer counts as a GM.");
  EXPECT_EQ(gm::PhraseRemovedReport("yo", false),
            "❌ `yo` is not a GM phrase.");
}

TEST(StreakReportTest, ReportsALiveStreak) {
  EXPECT_EQ(gm::StreakReport(Standing("luke", 1, 1), "<@1>"),
            "🔥 <@1>, your current streak is **1 day**!");
  EXPECT_EQ(gm::StreakReport(Standing("luke", 1, 2), "<@1>"),
            "🔥 <@1>, your current streak is **1 day**!\n"
            "Your best: **2 days**");
}

// Without a live streak the report says how to get one, and what there is to
// beat if they have had one before.
TEST(StreakReportTest, EncouragesSomeoneWithoutAStreak) {
  EXPECT_EQ(gm::StreakReport(Standing("", 0, 0), "<@1>"),
            "<@1>, you don't have an active streak. Say GM to start one! 🌅");
  EXPECT_EQ(gm::StreakReport(Standing("luke", 0, 2), "<@1>"),
            "<@1>, you don't have an active streak. Say GM to start one! 🌅\n"
            "Your best: **2 days**");
}

// Badges go by the best streak, so they show whether or not one is live.
TEST(StreakReportTest, ShowsTheBadgesHeld) {
  EXPECT_EQ(gm::StreakReport(Standing("luke", 7, 7), "<@1>"),
            "🔥 <@1>, your current streak is **7 days**!\n"
            "Badges: 🌱 🔥");
  EXPECT_EQ(gm::StreakReport(Standing("luke", 0, 3), "<@1>"),
            "<@1>, you don't have an active streak. Say GM to start one! 🌅\n"
            "Your best: **3 days**\n"
            "Badges: 🌱");
}

// /badges lists them all: those held, then what the others would take.
TEST(BadgesReportTest, ShowsEveryBadgeAndWhichAreHeld) {
  EXPECT_EQ(gm::BadgesReport(Standing("luke", 2, 8)),
            "🏅 **GM Streak Badges**\n"
            "Your best streak: **8 days**\n"
            "\n"
            "🌱 **Sprout** — 3 days ✅\n"
            "🔥 **Week Warrior** — 7 days ✅\n"
            "🔒 ~~Fortnight Star~~ — 14 days\n"
            "🔒 ~~Triple Week~~ — 21 days\n"
            "🔒 ~~Monthly Legend~~ — 30 days\n"
            "🔒 ~~Half Century~~ — 50 days\n"
            "🔒 ~~Diamond Dedication~~ — 75 days\n"
            "🔒 ~~Centurion~~ — 100 days\n"
            "🔒 ~~Dragon~~ — 150 days\n"
            "🔒 ~~Mythical~~ — 200 days\n"
            "🔒 ~~Year-Round Sun~~ — 365 days\n"
            "\n"
            "Earned: 2/11 badges");
}

// Someone who has never said GM gets the same list, all of it locked.
TEST(BadgesReportTest, ShowsANewcomerWhatThereIsToEarn) {
  const std::string report = gm::BadgesReport(Standing("", 0, 0));

  EXPECT_THAT(report, testing::HasSubstr("Your best streak: **0 days**"));
  EXPECT_THAT(report, testing::HasSubstr("🔒 ~~Sprout~~ — 3 days"));
  EXPECT_THAT(report, testing::EndsWith("Earned: 0/11 badges"));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //gm:report_test -- --benchmark_filter=all
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
//   ---------------------------------------------------------------
//   Benchmark                     Time             CPU   Iterations
//   ---------------------------------------------------------------
//   BM_LeaderboardReport       1807 ns         1805 ns       229448
//   BM_BadgesReport             399 ns          399 ns      1046295
// End of results.
// clang-format on

// A full leaderboard of ten.
void BM_LeaderboardReport(benchmark::State& state) {
  std::vector<gm::Standing> board;
  board.reserve(10);
  for (int place = 0; place < 10; ++place) {
    board.push_back(Standing("someone with a name", 30 - place, 40));
  }
  for (auto _ : state) benchmark::DoNotOptimize(gm::LeaderboardReport(board));
}
BENCHMARK(BM_LeaderboardReport);

// What /badges answers with.
void BM_BadgesReport(benchmark::State& state) {
  const gm::Standing standing = Standing("luke", 12, 40);
  for (auto _ : state) benchmark::DoNotOptimize(gm::BadgesReport(standing));
}
BENCHMARK(BM_BadgesReport);

}  // namespace
