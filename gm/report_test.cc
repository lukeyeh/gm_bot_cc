// What the bot says when asked about streaks, by example.

#include "gm/report.h"

#include <benchmark/benchmark.h>

#include <string>
#include <utility>
#include <vector>

#include "gm/ledger.h"
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
  const std::vector<std::string> phrases = {
      "gm",
      "good morning",
  };

  EXPECT_EQ(gm::PhraseListReport(phrases),
            "✅ **GM phrases**\n"
            "\n"
            "• `gm`\n"
            "• `good morning`\n"
            "\n"
            "Capitalisation does not matter.");
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
  EXPECT_EQ(gm::PhraseAddedReport("yo", true), "✅ `yo` now counts as a GM.");
  EXPECT_EQ(gm::PhraseAddedReport("gm", false), "`gm` already counts as a GM.");
  EXPECT_EQ(gm::PhraseRemovedReport("yo", true),
            "✅ `yo` no longer counts as a GM.");
  EXPECT_EQ(gm::PhraseRemovedReport("yo", false),
            "❌ `yo` is not a GM phrase.");
}

TEST(StreakReportTest, ReportsALiveStreak) {
  EXPECT_EQ(gm::StreakReport(Standing("luke", 1, 1), "<@1>"),
            "🔥 <@1>, your current streak is **1 day**!");
  EXPECT_EQ(gm::StreakReport(Standing("luke", 4, 9), "<@1>"),
            "🔥 <@1>, your current streak is **4 days**!\n"
            "Your best: **9 days**");
}

// Without a live streak the report says how to get one, and what there is to
// beat if they have had one before.
TEST(StreakReportTest, EncouragesSomeoneWithoutAStreak) {
  EXPECT_EQ(gm::StreakReport(Standing("", 0, 0), "<@1>"),
            "<@1>, you don't have an active streak. Say GM to start one! 🌅");
  EXPECT_EQ(gm::StreakReport(Standing("luke", 0, 7), "<@1>"),
            "<@1>, you don't have an active streak. Say GM to start one! 🌅\n"
            "Your best: **7 days**");
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
//   Date      2026-10-05
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
//   BM_LeaderboardReport       1830 ns         1789 ns       233936
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

}  // namespace
