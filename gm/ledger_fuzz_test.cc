// The ledger, checked against a model.
//
// The ledger works streaks out from ordered rows. The model here works them
// out the slow, obvious way, from sets of days, and keeps the hours a phrase
// counts during as the set of those hours. A fuzz test puts both through
// the same arbitrary sequence of GMs, forfeits, changes to the phrase list
// and messages written at some hour of the day, spread over two communities
// that must not affect each other, and requires them to agree on everything
// the ledger can be asked: each receipt, each forfeit, each member's
// standing, the leaderboard, whether each phrase was added or removed, the
// list of phrases with the hours of each, and what each message amounts to.
//
// Two smaller properties put the parts that need no ledger, a phrase's
// hours and the reading of a message, through many more inputs than a
// property that opens a database each time can get through.
//
// Badges have a model of their own, in badge_fuzz_test.cc.
//
//   bazel test //gm:ledger_fuzz_test
//       Tries each property on a few thousand random inputs.
//
//   bazel run --config=fuzz //gm:ledger_fuzz_test -- \
//       --fuzz=LedgerFuzzTest.AgreesWithTheModel --fuzz_for=60s
//       Searches for a counterexample for as long as asked, steered by which
//       code each input reaches. This is what it takes to go deep: the quick
//       mode gets through only a few hundred short histories.

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
#include <utility>
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
// text is added to or removed from the phrases, or it is a message they
// write in the GM channel, to be dealt with as whatever it amounts to.
struct Step {
  int who = 0;
  int day = 0;
  // The member's name, the phrase, or the message.
  std::string text;
  int kind = 0;
  // Which community it happens in.
  int where = 0;
  // The hour of the day a message is written during.
  int hour = 0;
  // The hours a phrase is added to count from and until. Not always ones
  // that make sense.
  int from = 0;
  int until = 0;

  bool is_forfeit() const { return kind == 0; }
  bool is_add_phrase() const { return kind == 5; }
  bool is_remove_phrase() const { return kind == 6; }
  bool is_message() const { return kind >= 7; }
};

// The name a member is recorded under when a message of theirs is a GM.
constexpr const char* kWriter = "writer";

// Text for a step: any bytes at all, or something from a short list, so that
// the same phrase comes up often enough to be added twice and removed.
auto AnyText() {
  return fuzztest::OneOf(fuzztest::Arbitrary<std::string>(),
                         fuzztest::ElementOf<std::string>({
                             "gm",
                             "GM",
                             "gm everyone",
                             "ok, yo",
                             "gmail",
                             "yoghurt",
                             "morning yo",
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
  int best_before = 0;
};

// A phrase, and the hours of the day during which it should count.
using ExpectedPhrase = std::pair<std::string, std::set<int>>;

// What a message should amount to.
struct ExpectedReading {
  gm::Reading::Kind kind = gm::Reading::Kind::kOther;
  // The phrase it says, if it says one.
  std::string phrase;
};

// One community's part of the ledger's behaviour, restated as simply as
// possible.
//
// A member has a series of lives, each a set of days with a GM. A forfeit
// ends the life they are on and starts an empty one. A streak is a run of
// consecutive days within one life.
class Model {
 public:
  ExpectedReceipt Record(int who, int day, const std::string& name) {
    Member& member = members_[who];
    // Their best as things stood, before this GM is added.
    const int best_before = StandingOf(who, day).best;

    member.name = name;
    const bool counted = member.lives[member.life].insert(day).second;

    // A record is set by the GM that makes the live streak one longer than
    // every other streak the member has had.
    const Summary summary = Summarise(member, day);
    return ExpectedReceipt{
        .counted = counted,
        .new_record = counted && summary.longest_other > 0 &&
                      summary.streak == summary.longest_other + 1,
        .standing = StandingOf(who, day),
        .best_before = best_before,
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

  // Every hour of the day.
  static std::set<int> AllDay() {
    std::set<int> hours;
    for (int hour = 0; hour < 24; ++hour) hours.insert(hour);
    return hours;
  }

  // The hours of the day from `from` until `until`, found by going round
  // the clock an hour at a time. Nothing if those are not hours.
  static std::optional<std::set<int>> HoursBetween(int from, int until) {
    if (from < 0 || from > 23 || until < 0 || until > 24) return std::nullopt;

    std::set<int> hours;
    int hour = from;
    do {
      hours.insert(hour);
      hour = (hour + 1) % 24;
    } while (hour != until % 24);
    return hours;
  }

  // Adding a phrase to count during `hours`. Nothing if it is not
  // acceptable as one; otherwise whether it was new. One that is there
  // already keeps the hours it had.
  std::optional<bool> AddPhrase(const std::string& text,
                                const std::set<int>& hours) {
    const std::optional<std::string> phrase = Phrase(text);
    if (!phrase.has_value()) return std::nullopt;

    return phrases_.emplace(*phrase, hours).second;
  }

  // What `message`, written during `hour`, amounts to: a GM if it says a
  // phrase that counts then; out of hours if it only says phrases that do
  // not, and then it is the first of them that is named.
  ExpectedReading Read(const std::string& message, int hour) const {
    const std::string text = Folded(message);

    ExpectedReading reading;
    for (const auto& [phrase, hours] : phrases_) {
      if (!Says(text, phrase)) continue;

      if (hours.contains(hour)) {
        return ExpectedReading{
            .kind = gm::Reading::Kind::kGm,
            .phrase = phrase,
        };
      }
      if (reading.kind == gm::Reading::Kind::kOther) {
        reading = ExpectedReading{
            .kind = gm::Reading::Kind::kOutOfHours,
            .phrase = phrase,
        };
      }
    }
    return reading;
  }

  // Removing a phrase: whether it was there.
  bool RemovePhrase(const std::string& text) {
    const std::optional<std::string> phrase = Phrase(text);
    return phrase.has_value() && phrases_.erase(*phrase) > 0;
  }

  // The phrases, in order, each with its hours.
  std::vector<ExpectedPhrase> Phrases() const {
    return std::vector<ExpectedPhrase>(phrases_.begin(), phrases_.end());
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

  // `text` as it is compared: trimmed of spaces and in lower case. Written
  // out longhand, like what follows, rather than by calling the code under
  // test.
  static std::string Folded(const std::string& text) {
    const auto is_space = [](char c) {
      return c == ' ' || (c >= '\t' && c <= '\r');
    };
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;

    std::string folded = text.substr(begin, end - begin);
    for (char& c : folded) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return folded;
  }

  // Whether `message`, folded, says `phrase`: is it, or opens or closes
  // with it where a letter or digit does not carry straight on.
  static bool Says(const std::string& message, const std::string& phrase) {
    const auto continues = [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9');
    };
    if (message == phrase) return true;
    if (message.size() < phrase.size()) return false;

    const size_t rest = message.size() - phrase.size();
    if (message.compare(0, phrase.size(), phrase) == 0 &&
        !continues(message[phrase.size()])) {
      return true;
    }
    return message.compare(rest, phrase.size(), phrase) == 0 &&
           !continues(message[rest - 1]);
  }

  // `text` as a phrase. Nothing if folding leaves it empty, over 50 bytes,
  // or with a control character or backtick.
  static std::optional<std::string> Phrase(const std::string& text) {
    std::string phrase = Folded(text);

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
  // Every ledger starts with these three, counting all day.
  std::map<std::string, std::set<int>> phrases_ = {
      {"gm", AllDay()},
      {"good morning", AllDay()},
      {"morning", AllDay()},
  };
};

ExpectedStanding Seen(const gm::Standing& standing) {
  return ExpectedStanding{
      .name = standing.member.name,
      .streak = standing.streak,
      .best = standing.best,
  };
}

// A phrase as the ledger has it, with its hours spelt out one by one.
ExpectedPhrase Seen(const gm::Phrase& phrase) {
  std::set<int> hours;
  for (int hour = 0; hour < 24; ++hour) {
    if (phrase.hours.Contains(hour)) hours.insert(hour);
  }
  return ExpectedPhrase(phrase.text, hours);
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

    // The member forfeits their streak, in the ledger and in the model.
    const auto forfeit = [&] {
      const absl::StatusOr<int> forfeited =
          community.Forfeit(kIds[step.who], Day(step.day));
      ASSERT_TRUE(forfeited.ok()) << forfeited.status();
      ASSERT_EQ(*forfeited, model.Forfeit(step.who, step.day));
    };

    // The member says GM under `name`, in the ledger and in the model.
    const auto record = [&](const std::string& name) {
      const absl::StatusOr<gm::Receipt> receipt = community.Record(
          gm::Member{
              .id = kIds[step.who],
              .name = name,
          },
          Day(step.day));
      ASSERT_TRUE(receipt.ok()) << receipt.status();

      const ExpectedReceipt expected = model.Record(step.who, step.day, name);
      ASSERT_EQ(receipt->counted, expected.counted);
      ASSERT_EQ(receipt->new_record, expected.new_record);
      ASSERT_EQ(Seen(receipt->standing), expected.standing);
      ASSERT_EQ(receipt->best_before, expected.best_before);
      ASSERT_EQ(receipt->standing.member.id, kIds[step.who]);
    };

    if (step.is_forfeit()) {
      forfeit();
      if (testing::Test::HasFatalFailure()) return;
      continue;
    }

    if (step.is_add_phrase()) {
      const absl::StatusOr<gm::Hours> hours =
          gm::Hours::Between(step.from, step.until);
      const std::optional<std::set<int>> expected_hours =
          Model::HoursBetween(step.from, step.until);
      if (!expected_hours.has_value()) {
        // Not hours: refused, and for that reason.
        ASSERT_TRUE(absl::IsInvalidArgument(hours.status())) << hours.status();
        continue;
      }
      ASSERT_TRUE(hours.ok()) << hours.status();

      const absl::StatusOr<bool> added = community.AddPhrase(step.text, *hours);
      const std::optional<bool> expected =
          model.AddPhrase(step.text, *expected_hours);
      if (!expected.has_value()) {
        // Not acceptable as a phrase: refused, and for that reason.
        ASSERT_TRUE(absl::IsInvalidArgument(added.status())) << added.status();
        continue;
      }
      ASSERT_TRUE(added.ok()) << added.status();
      ASSERT_EQ(*added, *expected);

      // What adding a phrase is for: a message saying just that is now
      // recognised, and if the phrase is new, counts from its first hour.
      const absl::StatusOr<std::vector<gm::Phrase>> phrases =
          community.Phrases();
      ASSERT_TRUE(phrases.ok()) << phrases.status();
      const gm::Reading reading = gm::Read(step.text, *phrases, step.from);
      ASSERT_NE(reading.kind, gm::Reading::Kind::kOther);
      if (*added) ASSERT_EQ(reading.kind, gm::Reading::Kind::kGm);
      continue;
    }

    if (step.is_message()) {
      // What the bot does with a message in a GM channel: a GM is recorded,
      // one out of hours changes nothing, and anything else costs the
      // streak.
      const absl::StatusOr<std::vector<gm::Phrase>> phrases =
          community.Phrases();
      ASSERT_TRUE(phrases.ok()) << phrases.status();
      const gm::Reading reading = gm::Read(step.text, *phrases, step.hour);

      const ExpectedReading expected = model.Read(step.text, step.hour);
      ASSERT_EQ(reading.kind, expected.kind);
      if (reading.phrase == nullptr) {
        ASSERT_EQ(expected.kind, gm::Reading::Kind::kOther);
      } else {
        ASSERT_EQ(reading.phrase->text, expected.phrase);
      }

      if (reading.kind == gm::Reading::Kind::kGm) record(kWriter);
      if (reading.kind == gm::Reading::Kind::kOther) forfeit();
      if (testing::Test::HasFatalFailure()) return;
      continue;
    }

    if (step.is_remove_phrase()) {
      const absl::StatusOr<bool> removed = community.RemovePhrase(step.text);
      ASSERT_TRUE(removed.ok()) << removed.status();
      ASSERT_EQ(*removed, model.RemovePhrase(step.text));
      continue;
    }

    record(step.text);
    if (testing::Test::HasFatalFailure()) return;
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

    const absl::StatusOr<std::vector<gm::Phrase>> phrases = community.Phrases();
    ASSERT_TRUE(phrases.ok()) << phrases.status();
    std::vector<ExpectedPhrase> seen_phrases;
    for (const gm::Phrase& phrase : *phrases) {
      seen_phrases.push_back(Seen(phrase));
    }
    ASSERT_EQ(seen_phrases, model.Phrases());
  }
}

// A few members over a few weeks, so that days collide and streaks form,
// break, join up and are forfeited, with any bytes at all for names, phrases
// and messages, every hour of the day, and hours for phrases that reach a
// little past what is allowed.
FUZZ_TEST(LedgerFuzzTest, AgreesWithTheModel)
    .WithDomains(fuzztest::VectorOf(
                     fuzztest::StructOf<Step>(
                         fuzztest::InRange(0, 3), fuzztest::InRange(0, 30),
                         AnyText(), fuzztest::InRange(0, 8),
                         fuzztest::InRange(0, 1), fuzztest::InRange(0, 23),
                         fuzztest::InRange(-1, 24), fuzztest::InRange(-1, 25)))
                     .WithMaxSize(80),
                 fuzztest::InRange(-2, 34), fuzztest::InRange(0, 6));

// Whatever two numbers are given as hours, they are hours exactly when the
// model says so, and then cover exactly the hours of the day that going
// round the clock from the first until the second passes through.
void HoursAgreeWithTheClock(int from, int until) {
  const absl::StatusOr<gm::Hours> hours = gm::Hours::Between(from, until);
  const std::optional<std::set<int>> expected =
      Model::HoursBetween(from, until);
  if (!expected.has_value()) {
    ASSERT_TRUE(absl::IsInvalidArgument(hours.status())) << hours.status();
    return;
  }
  ASSERT_TRUE(hours.ok()) << hours.status();

  for (int hour = 0; hour < 24; ++hour) {
    ASSERT_EQ(hours->Contains(hour), expected->contains(hour)) << hour;
  }
  ASSERT_EQ(hours->all_day(), expected->size() == 24);

  // Written the way people write them, they read back as the same hours.
  const absl::StatusOr<gm::Hours> parsed =
      gm::Hours::Parse(std::to_string(from) + "-" + std::to_string(until));
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  ASSERT_EQ(*parsed, *hours);
}

FUZZ_TEST(LedgerFuzzTest, HoursAgreeWithTheClock)
    .WithDomains(fuzztest::InRange(-3, 27), fuzztest::InRange(-3, 27));

// A phrase to put on a list: its text and the hours it counts between.
struct Listed {
  std::string text;
  int from = 0;
  int until = 0;
};

// Whatever phrases there are, with whatever hours, and whatever is written
// at whatever hour, the message amounts to what the model says it does.
void ReadsMessagesAsTheModelDoes(const std::vector<Listed>& listed,
                                 const std::string& message, int hour) {
  Model model;
  // The list as the ledger would hand it over: in order of text, starting
  // with the three every list starts with.
  std::map<std::string, gm::Hours> list = {
      {"gm", gm::Hours()},
      {"good morning", gm::Hours()},
      {"morning", gm::Hours()},
  };
  for (const Listed& entry : listed) {
    const absl::StatusOr<std::string> text = gm::CanonicalPhrase(entry.text);
    const std::optional<std::set<int>> hours =
        Model::HoursBetween(entry.from, entry.until);
    // The domain below gives only numbers that are hours.
    if (!hours.has_value()) continue;

    const std::optional<bool> added = model.AddPhrase(entry.text, *hours);
    ASSERT_EQ(text.ok(), added.has_value());
    if (!text.ok()) continue;

    const absl::StatusOr<gm::Hours> counted =
        gm::Hours::Between(entry.from, entry.until);
    ASSERT_TRUE(counted.ok()) << counted.status();
    list.emplace(*text, *counted);
  }
  // Leaving the usual three out half the time lets the others be heard.
  if (hour % 2 == 0) {
    for (const char* const usual : {"gm", "good morning", "morning"}) {
      list.erase(usual);
      model.RemovePhrase(usual);
    }
  }

  std::vector<gm::Phrase> phrases;
  phrases.reserve(list.size());
  for (const auto& [text, hours] : list) {
    phrases.push_back(gm::Phrase{
        .text = text,
        .hours = hours,
    });
  }

  const gm::Reading reading = gm::Read(message, phrases, hour);
  const ExpectedReading expected = model.Read(message, hour);
  ASSERT_EQ(reading.kind, expected.kind);
  if (reading.phrase == nullptr) {
    ASSERT_EQ(expected.kind, gm::Reading::Kind::kOther);
  } else {
    ASSERT_EQ(reading.phrase->text, expected.phrase);
  }
}

// Phrases and messages from a small vocabulary, so that messages say
// phrases, often several at once, and with hours that overlap, do not, and
// run past midnight.
FUZZ_TEST(LedgerFuzzTest, ReadsMessagesAsTheModelDoes)
    .WithDomains(fuzztest::VectorOf(fuzztest::StructOf<Listed>(
                                        fuzztest::ElementOf<std::string>({
                                            "yo",
                                            "yo yo",
                                            "YO there",
                                            "gn",
                                            "good",
                                            "good night",
                                            "night",
                                        }),
                                        fuzztest::InRange(0, 23),
                                        fuzztest::InRange(0, 24)))
                     .WithMaxSize(6),
                 fuzztest::OneOf(fuzztest::Arbitrary<std::string>(),
                                 fuzztest::ElementOf<std::string>({
                                     "yo",
                                     "Yo yo",
                                     "yo there",
                                     "yo there yo",
                                     "yoyo",
                                     "good night",
                                     "Good night, yo",
                                     "night night",
                                     "gn!",
                                     "ok gn",
                                     "gm",
                                     "good morning yo",
                                     "goodness",
                                 })),
                 fuzztest::InRange(0, 23));

}  // namespace
