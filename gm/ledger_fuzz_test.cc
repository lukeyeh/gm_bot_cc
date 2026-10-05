// The ledger, checked against a model.
//
// The ledger works streaks out in SQL. The model here works them out the
// slow, obvious way, from sets of days. A fuzz test puts both through the
// same arbitrary sequence of GMs, forfeits and changes to the phrase list,
// spread over two communities that must not affect each other, and requires
// them to agree on everything the ledger can be asked: each
// receipt, each forfeit, each member's standing, the leaderboard, whether
// each phrase was added or removed, and the list of phrases.
//
//   bazel test //gm:ledger_fuzz_test
//       Tries each property on a few thousand random inputs.
//
//   bazel run --config=fuzz //gm:ledger_fuzz_test -- \
//       --fuzz=LedgerFuzzTest.AgreesWithTheModel --fuzz_for=60s
//       Searches for a counterexample for as long as asked, steered by which
//       code each input reaches.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "fuzztest/fuzztest.h"
#include "gm/ledger.h"
#include "gm/phrase.h"
#include "gtest/gtest.h"

namespace {

// The members things happen to, by index. The ids span what a 64-bit id can
// be.
constexpr std::array<uint64_t, 4> kIds = {
    1,
    42,
    1234567890123456789,
    std::numeric_limits<uint64_t>::max(),
};

// The communities things happen in, by index.
constexpr std::array<uint64_t, 2> kCommunities = {
    1001,
    std::numeric_limits<uint64_t>::max() - 1,
};

// Days are counted from here, so that a few weeks of them cross a leap day
// and a month end.
absl::CivilDay Day(int number) { return absl::CivilDay(2024, 2, 10) + number; }

// One thing done to the ledger. Usually a member says GM on a day, under a
// name. Sometimes they forfeit their streak as of that day instead, or the
// text is added to or removed from the phrases.
struct Step {
  int who = 0;
  int day = 0;
  // The member's name, or the phrase.
  std::string text;
  int kind = 0;
  // Which community it happens in.
  int where = 0;

  bool is_forfeit() const { return kind == 0; }
  bool is_add_phrase() const { return kind == 5; }
  bool is_remove_phrase() const { return kind == 6; }
};

// Text for a step: any bytes at all, or something from a short list, so that
// the same phrase comes up often enough to be added twice and removed.
auto AnyText() {
  return fuzztest::OneOf(fuzztest::Arbitrary<std::string>(),
                         fuzztest::ElementOf<std::string>({
                             "gm",
                             "GM",
                             "  Good Morning ",
                             "morning",
                             "yo",
                             "Yo ",
                             "buenos días",
                             "",
                             "   ",
                             "good\nmorning",
                             "`gm`",
                             std::string(50, 'g'),
                             std::string(51, 'g'),
                         }));
}

// What a standing or a receipt should be, for comparing with the ledger's.
struct ExpectedStanding {
  std::string name;
  int streak = 0;
  int best = 0;

  friend bool operator==(const ExpectedStanding&,
                         const ExpectedStanding&) = default;
};

// gtest needs to be able to print one when a comparison fails.
void PrintTo(const ExpectedStanding& standing, std::ostream* out) {
  *out << "{name=" << testing::PrintToString(standing.name)
       << " streak=" << standing.streak << " best=" << standing.best << "}";
}

struct ExpectedReceipt {
  bool counted = false;
  bool new_record = false;
  ExpectedStanding standing;
};

// One community's part of the ledger's behaviour, restated as simply as
// possible.
//
// A member has a series of lives, each a set of days with a GM. A forfeit
// ends the life they are on and starts an empty one. A streak is a run of
// consecutive days within one life.
class Model {
 public:
  ExpectedReceipt Record(const Step& gm) {
    Member& member = members_[gm.who];
    member.name = gm.text;
    const bool counted = member.lives[member.life].insert(gm.day).second;

    // A record is set by the GM that makes the live streak one longer than
    // every other streak the member has had.
    const Summary summary = Summarise(member, gm.day);
    return ExpectedReceipt{
        .counted = counted,
        .new_record = counted && summary.longest_other > 0 &&
                      summary.streak == summary.longest_other + 1,
        .standing = StandingOf(gm.who, gm.day),
    };
  }

  // Returns the length of the streak given up.
  int Forfeit(int who, int today) {
    const auto found = members_.find(who);
    if (found == members_.end()) return 0;

    const int streak = Summarise(found->second, today).streak;
    if (streak > 0) ++found->second.life;
    return streak;
  }

  // Adding a phrase. Nothing if it is not acceptable as one; otherwise
  // whether it was new.
  std::optional<bool> AddPhrase(const std::string& text) {
    const std::optional<std::string> phrase = Phrase(text);
    if (!phrase.has_value()) return std::nullopt;

    return phrases_.insert(*phrase).second;
  }

  // Removing a phrase: whether it was there.
  bool RemovePhrase(const std::string& text) {
    const std::optional<std::string> phrase = Phrase(text);
    return phrase.has_value() && phrases_.erase(*phrase) > 0;
  }

  // The phrases, in order.
  std::vector<std::string> Phrases() const {
    return std::vector<std::string>(phrases_.begin(), phrases_.end());
  }

  // A member who had not said GM by `today` has no name and no streaks.
  ExpectedStanding StandingOf(int who, int today) const {
    const auto found = members_.find(who);
    if (found == members_.end()) return {};

    const Summary summary = Summarise(found->second, today);
    if (!summary.any) return {};

    return ExpectedStanding{
        .name = found->second.name,
        .streak = summary.streak,
        .best = std::max(summary.streak, summary.longest_other),
    };
  }

  std::vector<ExpectedStanding> Leaderboard(int today, size_t limit) const {
    std::vector<ExpectedStanding> board;
    for (const auto& [who, member] : members_) {
      if (Summarise(member, today).any) board.push_back(StandingOf(who, today));
    }

    // Longest live streak first, then best streak, then name.
    std::sort(board.begin(), board.end(),
              [](const ExpectedStanding& a, const ExpectedStanding& b) {
                return std::tie(b.streak, b.best, a.name) <
                       std::tie(a.streak, a.best, b.name);
              });
    board.resize(std::min(board.size(), limit));
    return board;
  }

 private:
  struct Member {
    std::string name;
    // Which life they are on, and the days with a GM in each life so far.
    int life = 0;
    std::map<int, std::set<int>> lives;
  };

  // A member's streaks as of some day.
  struct Summary {
    // Whether they had said GM at all by then.
    bool any = false;
    // The length of the live streak, or 0.
    int streak = 0;
    // The length of the longest streak other than the live one, or 0.
    int longest_other = 0;
  };

  static Summary Summarise(const Member& member, int today) {
    Summary summary;
    for (const auto& [life, days] : member.lives) {
      const std::vector<int> runs = RunsUpTo(days, today);
      if (runs.empty()) continue;
      summary.any = true;

      // Only the last run of the life they are on can be live, and only if
      // it reached today or yesterday.
      const int last_day = *std::prev(days.upper_bound(today));
      const bool ends_live = life == member.life && last_day >= today - 1;

      for (size_t i = 0; i < runs.size(); ++i) {
        if (ends_live && i + 1 == runs.size()) {
          summary.streak = runs[i];
        } else {
          summary.longest_other = std::max(summary.longest_other, runs[i]);
        }
      }
    }
    return summary;
  }

  // `text` as a phrase: trimmed of spaces and in lower case. Nothing if that
  // leaves it empty, over 50 bytes, or with a control character or backtick.
  // Written out longhand rather than by calling the code under test.
  static std::optional<std::string> Phrase(const std::string& text) {
    const auto is_space = [](char c) {
      return c == ' ' || (c >= '\t' && c <= '\r');
    };
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;

    std::string phrase = text.substr(begin, end - begin);
    for (char& c : phrase) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }

    if (phrase.empty() || phrase.size() > 50) return std::nullopt;
    for (const char c : phrase) {
      const auto byte = static_cast<unsigned char>(c);
      if (byte < 0x20 || byte == 0x7F || c == '`') return std::nullopt;
    }
    return phrase;
  }

  // The lengths of the runs of consecutive days among those of `days` that
  // are no later than `today`, earliest run first.
  static std::vector<int> RunsUpTo(const std::set<int>& days, int today) {
    std::vector<int> runs;
    int previous = 0;
    for (const int day : days) {
      if (day > today) break;

      if (!runs.empty() && day == previous + 1) {
        ++runs.back();
      } else {
        runs.push_back(1);
      }
      previous = day;
    }
    return runs;
  }

  std::map<int, Member> members_;
  // Every ledger starts with these three.
  std::set<std::string> phrases_ = {
      "gm",
      "good morning",
      "morning",
  };
};

ExpectedStanding Seen(const gm::Standing& standing) {
  return ExpectedStanding{
      .name = standing.member.name,
      .streak = standing.streak,
      .best = standing.best,
  };
}

// Whatever happens, in whatever order and in whichever community, and
// whatever day the ledger is then asked about, it says what the model says.
void AgreesWithTheModel(const std::vector<Step>& steps, int today, int limit) {
  absl::StatusOr<gm::Ledger> ledger = gm::Ledger::InMemory();
  ASSERT_TRUE(ledger.ok()) << ledger.status();
  // A model per community: if one community's steps leaked into another,
  // the ledger would stop agreeing with them.
  std::array<Model, kCommunities.size()> models;

  for (const Step& step : steps) {
    gm::Community community = ledger->community(kCommunities[step.where]);
    Model& model = models[step.where];

    if (step.is_forfeit()) {
      const absl::StatusOr<int> forfeited =
          community.Forfeit(kIds[step.who], Day(step.day));
      ASSERT_TRUE(forfeited.ok()) << forfeited.status();
      ASSERT_EQ(*forfeited, model.Forfeit(step.who, step.day));
      continue;
    }

    if (step.is_add_phrase()) {
      const absl::StatusOr<bool> added = community.AddPhrase(step.text);
      const std::optional<bool> expected = model.AddPhrase(step.text);
      if (!expected.has_value()) {
        // Not acceptable as a phrase: refused, and for that reason.
        ASSERT_TRUE(absl::IsInvalidArgument(added.status())) << added.status();
        continue;
      }
      ASSERT_TRUE(added.ok()) << added.status();
      ASSERT_EQ(*added, *expected);

      // What adding a phrase is for: a message saying just that now counts.
      const absl::StatusOr<std::vector<std::string>> phrases =
          community.Phrases();
      ASSERT_TRUE(phrases.ok()) << phrases.status();
      ASSERT_TRUE(gm::IsGm(step.text, *phrases));
      continue;
    }

    if (step.is_remove_phrase()) {
      const absl::StatusOr<bool> removed = community.RemovePhrase(step.text);
      ASSERT_TRUE(removed.ok()) << removed.status();
      ASSERT_EQ(*removed, model.RemovePhrase(step.text));
      continue;
    }

    const absl::StatusOr<gm::Receipt> receipt = community.Record(
        gm::Member{
            .id = kIds[step.who],
            .name = step.text,
        },
        Day(step.day));
    ASSERT_TRUE(receipt.ok()) << receipt.status();

    const ExpectedReceipt expected = model.Record(step);
    ASSERT_EQ(receipt->counted, expected.counted);
    ASSERT_EQ(receipt->new_record, expected.new_record);
    ASSERT_EQ(Seen(receipt->standing), expected.standing);
    ASSERT_EQ(receipt->standing.member.id, kIds[step.who]);
  }

  for (size_t where = 0; where < kCommunities.size(); ++where) {
    gm::Community community = ledger->community(kCommunities[where]);
    const Model& model = models[where];

    for (int who = 0; who < static_cast<int>(kIds.size()); ++who) {
      const absl::StatusOr<gm::Standing> standing =
          community.StandingOf(kIds[who], Day(today));
      ASSERT_TRUE(standing.ok()) << standing.status();
      ASSERT_EQ(Seen(*standing), model.StandingOf(who, today));
    }

    const absl::StatusOr<std::vector<gm::Standing>> board =
        community.Leaderboard(Day(today), limit);
    ASSERT_TRUE(board.ok()) << board.status();
    std::vector<ExpectedStanding> seen;
    for (const gm::Standing& standing : *board) seen.push_back(Seen(standing));
    ASSERT_EQ(seen, model.Leaderboard(today, static_cast<size_t>(limit)));

    const absl::StatusOr<std::vector<std::string>> phrases =
        community.Phrases();
    ASSERT_TRUE(phrases.ok()) << phrases.status();
    ASSERT_EQ(*phrases, model.Phrases());
  }
}

// A few members over a few weeks, so that days collide and streaks form,
// break, join up and are forfeited, with any bytes at all for names and for
// phrases.
FUZZ_TEST(LedgerFuzzTest, AgreesWithTheModel)
    .WithDomains(fuzztest::VectorOf(fuzztest::StructOf<Step>(
                                        fuzztest::InRange(0, 3),
                                        fuzztest::InRange(0, 30), AnyText(),
                                        fuzztest::InRange(0, 6),
                                        fuzztest::InRange(0, 1)))
                     .WithMaxSize(80),
                 fuzztest::InRange(-2, 34), fuzztest::InRange(0, 6));

}  // namespace
