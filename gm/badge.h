// Badges: what a member is awarded for how long a streak they have managed.
//
// A badge is earned by a best streak reaching its length, and is kept for
// good: losing the streak does not lose the badge. So which badges someone
// holds follows from their best streak alone, and nothing about badges is
// stored.

#ifndef GM_BADGE_H_
#define GM_BADGE_H_

#include <span>
#include <string_view>

namespace gm {

struct Badge {
  // The length of streak, in days, that earns it.
  int days = 0;
  std::string_view emoji;
  std::string_view name;
};

// Every badge there is, from the easiest to earn to the hardest.
std::span<const Badge> AllBadges();

// The badges held by someone whose best streak is `best` days: the easiest
// so many of AllBadges().
std::span<const Badge> BadgesEarnedBy(int best);

// The badge that a best streak rising from `best_before` days to `best`
// earns, or null if it earns none. A GM lengthens a streak by a day at most,
// so it never earns more than one; if a longer rise crosses several, this is
// the hardest of them.
const Badge* BadgeNewlyEarned(int best_before, int best);

}  // namespace gm

#endif  // GM_BADGE_H_
