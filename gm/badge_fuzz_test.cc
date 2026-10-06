// Badges, checked against a model.
//
// The bot never stores who holds which badge: it works that out from a best
// streak each time. The model here does what the bot this one replaces did
// instead. It keeps a counter per streak and a set of badges held, and hands
// a badge over on the day the counter reaches its length. Fuzz tests require
// the two to agree, on arbitrary numbers and on arbitrary histories of GMs,
// missed days and forfeits.
//
//   bazel test //gm:badge_fuzz_test
//       Tries each property on a few thousand random inputs.
//
//   bazel run --config=fuzz //gm:badge_fuzz_test -- \
//       --fuzz=BadgeFuzzTest.AwardsWhatACounterWould --fuzz_for=60s
//       Searches for a counterexample for as long as asked.

#include <array>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "fuzztest/fuzztest.h"
#include "gm/badge.h"
#include "gm/greeting.h"
#include "gm/ledger.h"
#include "gtest/gtest.h"

namespace {

// The badges as the model knows them: the length of streak and the name,
// easiest first. Written out again rather than read from the code under
// test.
constexpr std::array<std::pair<int, const char*>, 11> kExpected = {{
    {3, "Sprout"},
    {7, "Week Warrior"},
    {14, "Fortnight Star"},
    {21, "Triple Week"},
    {30, "Monthly Legend"},
    {50, "Half Century"},
    {75, "Diamond Dedication"},
    {100, "Centurion"},
    {150, "Dragon"},
    {200, "Mythical"},
    {365, "Year-Round Sun"},
}};

std::vector<std::string> Names(std::span<const gm::Badge> badges) {
  std::vector<std::string> names;
  for (const gm::Badge& badge : badges) names.emplace_back(badge.name);
  return names;
}

// Whatever a best streak is and however far it rises, the badges held are
// the ones the table gives for it, and the one newly earned is the hardest
// whose length the rise reached or passed.
void HoldsWhatTheTableSays(int best_before, int rise) {
  const int best = best_before + rise;

  std::vector<std::string> held;
  std::optional<std::string> newly;
  for (const auto& [days, name] : kExpected) {
    if (days <= best) held.emplace_back(name);
    if (days > best_before && days <= best) newly = name;
  }

  ASSERT_EQ(Names(gm::BadgesEarnedBy(best)), held);

  const gm::Badge* const earned = gm::BadgeNewlyEarned(best_before, best);
  if (!newly.has_value()) {
    ASSERT_EQ(earned, nullptr);
    return;
  }
  ASSERT_NE(earned, nullptr);
  ASSERT_EQ(earned->name, *newly);
}

FUZZ_TEST(BadgeFuzzTest, HoldsWhatTheTableSays)
    .WithDomains(fuzztest::InRange(-5, 500), fuzztest::InRange(0, 500));

// A stretch of one member's history: a GM a day for some days, then perhaps
// a forfeit, then some days missed.
struct Stretch {
  int days = 0;
  bool forfeit = false;
  int missed = 0;
};

// A member's badges kept the way a bot with counters would keep them.
class Counter {
 public:
  // The member says GM on a day they had not yet; `consecutive` is whether
  // it is the day after their last. Returns the name of the badge this wins
  // them, if it wins one.
  std::optional<std::string> Gm(bool consecutive) {
    streak_ = consecutive ? streak_ + 1 : 1;

    for (const auto& [days, name] : kExpected) {
      // Won on the day the streak reaches its length, and only once.
      if (days == streak_ && held_.insert(days).second) return name;
    }
    return std::nullopt;
  }

  void Forfeit() { streak_ = 0; }

  int streak() const { return streak_; }

  // The names of the badges held, easiest first.
  std::vector<std::string> Held() const {
    std::vector<std::string> names;
    for (const auto& [days, name] : kExpected) {
      if (held_.contains(days)) names.emplace_back(name);
    }
    return names;
  }

 private:
  int streak_ = 0;
  std::set<int> held_;
};

// Through any history, the badges the ledger's receipts imply are the ones
// the counter holds, and the bot announces a badge on exactly the GMs where
// the counter hands one over.
void AwardsWhatACounterWould(const std::vector<Stretch>& history) {
  absl::StatusOr<gm::Ledger> ledger = gm::Ledger::InMemory();
  ASSERT_TRUE(ledger.ok()) << ledger.status();
  gm::Community community = ledger->community(1);
  const gm::Member member = {
      .id = 7,
      .name = "luke",
  };
  Counter counter;

  absl::CivilDay day(2024, 1, 1);
  bool consecutive = false;
  for (const Stretch& stretch : history) {
    for (int i = 0; i < stretch.days; ++i) {
      const absl::StatusOr<gm::Receipt> receipt = community.Record(member, day);
      ASSERT_TRUE(receipt.ok()) << receipt.status();
      const std::optional<std::string> won = counter.Gm(consecutive);

      ASSERT_EQ(receipt->standing.streak, counter.streak());
      ASSERT_EQ(Names(gm::BadgesEarnedBy(receipt->standing.best)),
                counter.Held());

      const std::string announced =
          gm::Announcement(*receipt, "<@7>").value_or("");
      if (won.has_value()) {
        ASSERT_TRUE(absl::StrContains(
            announced, absl::StrCat("earned the **", *won, "** badge")))
            << announced;
      } else {
        ASSERT_FALSE(absl::StrContains(announced, "BADGE")) << announced;
      }

      ++day;
      consecutive = true;
    }

    if (stretch.forfeit) {
      // As of the day of the last GM, so that there is a streak to lose.
      ASSERT_TRUE(community.Forfeit(member.id, day - 1).ok());
      counter.Forfeit();
    }
    if (stretch.missed > 0) {
      day += stretch.missed;
      consecutive = false;
    }
  }
}

// Stretches long enough to reach the first several badges, and to reach
// them a second time after a break, when they must not be won again.
FUZZ_TEST(BadgeFuzzTest, AwardsWhatACounterWould)
    .WithDomains(fuzztest::VectorOf(
                     fuzztest::StructOf<Stretch>(fuzztest::InRange(1, 60),
                                                 fuzztest::Arbitrary<bool>(),
                                                 fuzztest::InRange(0, 3)))
                     .WithMaxSize(6));

}  // namespace
