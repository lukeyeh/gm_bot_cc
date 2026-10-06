#include "gm/badge.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace gm {
namespace {

// In order of length, which the functions below rely on.
constexpr std::array<Badge, 11> kBadges = {{
    {
        .days = 3,
        .emoji = "🌱",
        .name = "Sprout",
    },
    {
        .days = 7,
        .emoji = "🔥",
        .name = "Week Warrior",
    },
    {
        .days = 14,
        .emoji = "⭐",
        .name = "Fortnight Star",
    },
    {
        .days = 21,
        .emoji = "🌟",
        .name = "Triple Week",
    },
    {
        .days = 30,
        .emoji = "💎",
        .name = "Monthly Legend",
    },
    {
        .days = 50,
        .emoji = "👑",
        .name = "Half Century",
    },
    {
        .days = 75,
        .emoji = "🏅",
        .name = "Diamond Dedication",
    },
    {
        .days = 100,
        .emoji = "🏆",
        .name = "Centurion",
    },
    {
        .days = 150,
        .emoji = "🐉",
        .name = "Dragon",
    },
    {
        .days = 200,
        .emoji = "🌈",
        .name = "Mythical",
    },
    {
        .days = 365,
        .emoji = "☀️",
        .name = "Year-Round Sun",
    },
}};

// How many badges a best streak of `best` days has earned.
size_t CountEarnedBy(int best) {
  return static_cast<size_t>(
      std::ranges::upper_bound(kBadges, best, {}, &Badge::days) -
      kBadges.begin());
}

}  // namespace

std::span<const Badge> AllBadges() { return kBadges; }

std::span<const Badge> BadgesEarnedBy(int best) {
  return std::span<const Badge>(kBadges).first(CountEarnedBy(best));
}

const Badge* BadgeNewlyEarned(int best_before, int best) {
  const size_t earned = CountEarnedBy(best);
  if (earned == CountEarnedBy(best_before)) return nullptr;

  return &kBadges[earned - 1];
}

}  // namespace gm
