// Badges by example: which there are, who holds which, and when one is won.

#include "gm/badge.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using gm::Badge;
using testing::ElementsAre;
using testing::IsEmpty;

// The names of `badges`, for comparing.
std::vector<std::string> Names(std::span<const Badge> badges) {
  std::vector<std::string> names;
  names.reserve(badges.size());
  for (const Badge& badge : badges) names.emplace_back(badge.name);
  return names;
}

// The full set, from three days to a year.
TEST(BadgeTest, ThereAreElevenFromEasiestToHardest) {
  const std::span<const Badge> all = gm::AllBadges();

  ASSERT_EQ(all.size(), 11);
  EXPECT_EQ(all.front().name, "Sprout");
  EXPECT_EQ(all.front().days, 3);
  EXPECT_EQ(all.front().emoji, "🌱");
  EXPECT_EQ(all.back().name, "Year-Round Sun");
  EXPECT_EQ(all.back().days, 365);

  for (size_t i = 1; i < all.size(); ++i) {
    EXPECT_LT(all[i - 1].days, all[i].days);
  }
}

// What someone holds depends only on their best streak, and a badge is held
// from the very day its length is reached.
TEST(BadgeTest, AreHeldForTheBestStreakReached) {
  EXPECT_THAT(Names(gm::BadgesEarnedBy(0)), IsEmpty());
  EXPECT_THAT(Names(gm::BadgesEarnedBy(2)), IsEmpty());
  EXPECT_THAT(Names(gm::BadgesEarnedBy(3)), ElementsAre("Sprout"));
  EXPECT_THAT(Names(gm::BadgesEarnedBy(6)), ElementsAre("Sprout"));
  EXPECT_THAT(Names(gm::BadgesEarnedBy(20)),
              ElementsAre("Sprout", "Week Warrior", "Fortnight Star"));
  EXPECT_EQ(gm::BadgesEarnedBy(365).size(), 11);
  EXPECT_EQ(gm::BadgesEarnedBy(10'000).size(), 11);
}

// A badge is won by the GM that takes a best streak to its length.
TEST(BadgeTest, IsNewlyEarnedWhenTheBestReachesItsLength) {
  const Badge* const sprout = gm::BadgeNewlyEarned(2, 3);
  ASSERT_NE(sprout, nullptr);
  EXPECT_EQ(sprout->name, "Sprout");

  const Badge* const centurion = gm::BadgeNewlyEarned(99, 100);
  ASSERT_NE(centurion, nullptr);
  EXPECT_EQ(centurion->name, "Centurion");
}

// Most GMs win nothing: the best did not rise, or rose between two lengths.
// In particular a badge already held is not won again by a later streak
// passing the same length, because that does not raise the best.
TEST(BadgeTest, IsNotEarnedOtherwise) {
  EXPECT_EQ(gm::BadgeNewlyEarned(3, 4), nullptr);
  EXPECT_EQ(gm::BadgeNewlyEarned(0, 1), nullptr);
  EXPECT_EQ(gm::BadgeNewlyEarned(30, 30), nullptr);
  EXPECT_EQ(gm::BadgeNewlyEarned(400, 401), nullptr);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //gm:badge_test -- --benchmark_filter=all
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
//   --------------------------------------------------------------
//   Benchmark                    Time             CPU   Iterations
//   --------------------------------------------------------------
//   BM_BadgeNewlyEarned       8.15 ns         8.14 ns     52042202
// End of results.
// clang-format on

// The check made on every GM that counts.
void BM_BadgeNewlyEarned(benchmark::State& state) {
  int best = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(gm::BadgeNewlyEarned(best, best + 1));
    best = (best + 1) % 400;
  }
}
BENCHMARK(BM_BadgeNewlyEarned);

}  // namespace
