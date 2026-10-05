// Shows how gm::Ledger turns a record of GMs into streaks.

#include "gm/ledger.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "sqlite/database.h"

namespace gm {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;

// October 2026, by day of the month.
absl::CivilDay October(const int day) { return absl::CivilDay(2026, 10, day); }

const Member kLuke{
    .id = 1,
    .name = "luke",
};
const Member kAda{
    .id = 2,
    .name = "ada",
};

// The community most tests happen in. Any number would do.
constexpr uint64_t kHere = 1001;
constexpr uint64_t kElsewhere = 2002;

Ledger NewLedger() {
  absl::StatusOr<Ledger> ledger = Ledger::InMemory();
  ABSL_CHECK_OK(ledger);
  return std::move(*ledger);
}

// Records a GM that the test expects to succeed.
Receipt Gm(Community community, const Member& member,
           const absl::CivilDay day) {
  const absl::StatusOr<Receipt> receipt = community.Record(member, day);
  ABSL_CHECK_OK(receipt);
  return *receipt;
}

// The heart of it: a GM on each consecutive day extends the streak by one.
TEST(LedgerTest, ConsecutiveDaysBuildAStreak) {
  Ledger ledger = NewLedger();
  const Community here = ledger.community(kHere);

  EXPECT_EQ(Gm(here, kLuke, October(1)).standing.streak, 1);
  EXPECT_EQ(Gm(here, kLuke, October(2)).standing.streak, 2);
  const Receipt third = Gm(here, kLuke, October(3));

  EXPECT_TRUE(third.counted);
  EXPECT_EQ(third.standing.streak, 3);
  EXPECT_EQ(third.standing.best, 3);
  EXPECT_EQ(third.standing.member.name, "luke");
}

// A second GM on the same day is acknowledged but not counted.
TEST(LedgerTest, OnlyTheFirstGmOfADayCounts) {
  Ledger ledger = NewLedger();
  const Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));

  const Receipt again = Gm(here, kLuke, October(1));

  EXPECT_FALSE(again.counted);
  EXPECT_EQ(again.standing.streak, 1);
}

// Missing a day ends the streak; the next GM starts a new one, and the old
// one is remembered as the best.
TEST(LedgerTest, AMissedDayStartsTheStreakOver) {
  Ledger ledger = NewLedger();
  const Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  Gm(here, kLuke, October(2));
  Gm(here, kLuke, October(3));

  const Receipt after_gap = Gm(here, kLuke, October(5));

  EXPECT_EQ(after_gap.standing.streak, 1);
  EXPECT_EQ(after_gap.standing.best, 3);
}

// Streaks are a matter of dates, not of months: they run across month ends.
TEST(LedgerTest, StreaksCrossMonthBoundaries) {
  Ledger ledger = NewLedger();
  const Community here = ledger.community(kHere);
  Gm(here, kLuke, absl::CivilDay(2026, 2, 28));

  EXPECT_EQ(Gm(here, kLuke, absl::CivilDay(2026, 3, 1)).standing.streak, 2);
}

// A streak is still alive the day after its last GM, when it can still be
// extended, and is gone the day after that. Nothing has to be recorded for it
// to end.
TEST(LedgerTest, AStreakLapsesOnceAWholeDayIsMissed) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  Gm(here, kLuke, October(2));

  const absl::StatusOr<Standing> next_day =
      here.StandingOf(kLuke.id, October(3));
  ABSL_ASSERT_OK(next_day);
  EXPECT_EQ(next_day->streak, 2);

  const absl::StatusOr<Standing> day_after =
      here.StandingOf(kLuke.id, October(4));
  ABSL_ASSERT_OK(day_after);
  EXPECT_EQ(day_after->streak, 0);
  EXPECT_EQ(day_after->best, 2);
}

// Standings are as of a day. A GM recorded for a later day, as a clock set
// back can leave behind, is not part of them until that day arrives. (Found
// by ledger_fuzz_test.)
TEST(LedgerTest, AGmDoesNotCountBeforeItsDay) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Gm(here, kLuke, October(10));
  Gm(here, kLuke, October(11));

  const absl::StatusOr<Standing> before = here.StandingOf(kLuke.id, October(5));
  ABSL_ASSERT_OK(before);
  EXPECT_EQ(before->streak, 0);
  EXPECT_EQ(before->best, 0);

  const absl::StatusOr<std::vector<Standing>> board =
      here.Leaderboard(October(5), 10);
  ABSL_ASSERT_OK(board);
  EXPECT_TRUE(board->empty());

  // Part of the way through, only the part so far counts.
  const absl::StatusOr<Standing> midway =
      here.StandingOf(kLuke.id, October(10));
  ABSL_ASSERT_OK(midway);
  EXPECT_EQ(midway->streak, 1);
}

// A forfeit ends the live streak on the spot, and says how much was lost.
// What was achieved is not taken away: it is still the member's best.
TEST(LedgerTest, ForfeitEndsTheLiveStreak) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  Gm(here, kLuke, October(2));
  Gm(here, kLuke, October(3));

  EXPECT_THAT(here.Forfeit(kLuke.id, October(3)), IsOkAndHolds(3));

  const absl::StatusOr<Standing> standing =
      here.StandingOf(kLuke.id, October(3));
  ABSL_ASSERT_OK(standing);
  EXPECT_EQ(standing->streak, 0);
  EXPECT_EQ(standing->best, 3);
}

// After a forfeit the next GM starts from one, even on a day that already
// had a GM before the forfeit.
TEST(LedgerTest, AGmAfterAForfeitStartsOver) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  Gm(here, kLuke, October(2));
  ABSL_ASSERT_OK(here.Forfeit(kLuke.id, October(2)));

  const Receipt same_day = Gm(here, kLuke, October(2));
  EXPECT_TRUE(same_day.counted);
  EXPECT_EQ(same_day.standing.streak, 1);
  EXPECT_EQ(same_day.standing.best, 2);

  // And it builds from there as any streak does.
  EXPECT_EQ(Gm(here, kLuke, October(3)).standing.streak, 2);
}

// There has to be a live streak to lose. Without one a forfeit changes
// nothing, so it cannot be used up before a streak begins.
TEST(LedgerTest, ForfeitWithoutALiveStreakDoesNothing) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  EXPECT_THAT(here.Forfeit(kLuke.id, October(1)), IsOkAndHolds(0));

  Gm(here, kLuke, October(1));
  Gm(here, kLuke, October(2));
  // By the 5th the streak has lapsed of its own accord.
  EXPECT_THAT(here.Forfeit(kLuke.id, October(5)), IsOkAndHolds(0));

  const absl::StatusOr<Standing> standing =
      here.StandingOf(kLuke.id, October(5));
  ABSL_ASSERT_OK(standing);
  EXPECT_EQ(standing->best, 2);
}

// A forfeited streak is still the record to beat.
TEST(LedgerTest, AForfeitedStreakIsStillTheRecordToBeat) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  Gm(here, kLuke, October(2));
  ABSL_ASSERT_OK(here.Forfeit(kLuke.id, October(2)));

  EXPECT_FALSE(Gm(here, kLuke, October(3)).new_record);
  EXPECT_FALSE(Gm(here, kLuke, October(4)).new_record);  // Equals it.
  EXPECT_TRUE(Gm(here, kLuke, October(5)).new_record);   // Passes it.
}

// One member's forfeit is nobody else's.
TEST(LedgerTest, ForfeitAffectsOnlyThatMember) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  Gm(here, kAda, October(1));

  ABSL_ASSERT_OK(here.Forfeit(kLuke.id, October(1)));

  const absl::StatusOr<Standing> ada = here.StandingOf(kAda.id, October(1));
  ABSL_ASSERT_OK(ada);
  EXPECT_EQ(ada->streak, 1);
}

// A record is announced once: on the GM that first takes a streak past the
// previous best. Not during a first streak, and not on the days that follow.
TEST(LedgerTest, NewRecordIsTheGmThatPassesThePreviousBest) {
  Ledger ledger = NewLedger();
  const Community here = ledger.community(kHere);
  // A first streak of two days sets a record without beating one.
  EXPECT_FALSE(Gm(here, kLuke, October(1)).new_record);
  EXPECT_FALSE(Gm(here, kLuke, October(2)).new_record);

  // After a gap, a second streak.
  EXPECT_FALSE(Gm(here, kLuke, October(10)).new_record);
  EXPECT_FALSE(Gm(here, kLuke, October(11)).new_record);  // Equals it.
  EXPECT_TRUE(Gm(here, kLuke, October(12)).new_record);   // Passes it.
  EXPECT_FALSE(Gm(here, kLuke, October(12)).new_record);  // A repeat.
  EXPECT_FALSE(Gm(here, kLuke, October(13)).new_record);  // Already ahead.
}

// Someone the ledger has never heard of simply has no streak.
TEST(LedgerTest, UnknownMembersHaveAZeroStanding) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);

  const absl::StatusOr<Standing> standing = here.StandingOf(99, October(1));

  ABSL_ASSERT_OK(standing);
  EXPECT_EQ(standing->member.id, 99);
  EXPECT_EQ(standing->streak, 0);
  EXPECT_EQ(standing->best, 0);
}

// Members are known by the name they last said GM under.
TEST(LedgerTest, RemembersTheLatestName) {
  Ledger ledger = NewLedger();
  const Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  const Member renamed{
      .id = kLuke.id,
      .name = "luke the early",
  };

  EXPECT_EQ(Gm(here, renamed, October(2)).standing.member.name,
            "luke the early");
}

// The leaderboard ranks by live streak, then best streak, then name.
TEST(LedgerTest, LeaderboardRanksByLiveStreak) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  const Member grace{
      .id = 3,
      .name = "grace",
  };
  const Member alan{
      .id = 4,
      .name = "alan",
  };
  // Luke: a long streak that has lapsed, and a live one of one day.
  for (int day = 1; day <= 5; ++day) {
    Gm(here, kLuke, October(day));
  }
  Gm(here, kLuke, October(10));
  // Ada: a live streak of three.
  for (int day = 8; day <= 10; ++day) {
    Gm(here, kAda, October(day));
  }
  // Grace and Alan: one day each, yesterday.
  Gm(here, grace, October(9));
  Gm(here, alan, October(9));

  const absl::StatusOr<std::vector<Standing>> board =
      here.Leaderboard(October(10), 10);

  ABSL_ASSERT_OK(board);
  std::vector<std::string> names;
  for (const Standing& standing : *board) {
    names.push_back(standing.member.name);
  }
  // Ada leads on streak. The rest are tied on one day: Luke's best of five
  // puts him ahead, and Alan precedes Grace alphabetically.
  EXPECT_THAT(names, ElementsAre("ada", "luke", "alan", "grace"));
  EXPECT_EQ((*board)[1].streak, 1);
  EXPECT_EQ((*board)[1].best, 5);
}

TEST(LedgerTest, LeaderboardHonoursItsLimit) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  Gm(here, kAda, October(1));

  const absl::StatusOr<std::vector<Standing>> board =
      here.Leaderboard(October(1), 1);

  ABSL_ASSERT_OK(board);
  EXPECT_EQ(board->size(), 1);
}

// Discord ids use the full 64 bits' worth of digits; they survive storage.
TEST(LedgerTest, KeepsLargeIdsIntact) {
  Ledger ledger = NewLedger();
  const Community here = ledger.community(kHere);
  const Member member{
      .id = 1234567890123456789,
      .name = "snowflake",
  };

  EXPECT_EQ(Gm(here, member, October(1)).standing.member.id,
            1234567890123456789);
}

// The point of the database: streaks outlive the process.
TEST(LedgerTest, PersistsAcrossReopening) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "gm.db";
  {
    absl::StatusOr<Ledger> ledger = Ledger::Open(file);
    ABSL_ASSERT_OK(ledger);
    Gm(ledger->community(kHere), kLuke, October(1));
  }

  absl::StatusOr<Ledger> reopened = Ledger::Open(file);
  ABSL_ASSERT_OK(reopened);
  EXPECT_EQ(Gm(reopened->community(kHere), kLuke, October(2)).standing.streak,
            2);
}

// Communities share nothing. The same person saying GM in two of them has a
// streak in each, and each has its own leaderboard and its own phrases.
TEST(LedgerTest, CommunitiesAreSeparate) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Community elsewhere = ledger.community(kElsewhere);

  Gm(here, kLuke, October(1));
  Gm(here, kLuke, October(2));
  EXPECT_EQ(Gm(elsewhere, kLuke, October(2)).standing.streak, 1);

  // A forfeit in one leaves the other alone.
  EXPECT_THAT(elsewhere.Forfeit(kLuke.id, October(2)), IsOkAndHolds(1));
  const absl::StatusOr<Standing> standing =
      here.StandingOf(kLuke.id, October(2));
  ABSL_ASSERT_OK(standing);
  EXPECT_EQ(standing->streak, 2);

  // So does a change to the phrases.
  ABSL_ASSERT_OK(elsewhere.AddPhrase("yo"));
  EXPECT_THAT(here.Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "morning")));
  EXPECT_THAT(elsewhere.Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "morning", "yo")));

  // And someone who has only said GM elsewhere is not on the board here.
  Gm(elsewhere, kAda, October(2));
  const absl::StatusOr<std::vector<Standing>> board =
      here.Leaderboard(October(2), 10);
  ABSL_ASSERT_OK(board);
  EXPECT_EQ(board->size(), 1);
}

// A new ledger counts the usual phrases, and the list can be changed.
TEST(LedgerTest, KeepsTheListOfPhrases) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  EXPECT_THAT(here.Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "morning")));

  EXPECT_THAT(here.AddPhrase("yo"), IsOkAndHolds(true));
  EXPECT_THAT(here.RemovePhrase("morning"), IsOkAndHolds(true));

  EXPECT_THAT(here.Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "yo")));
}

// However a phrase is typed, it is the same phrase: adding it twice adds it
// once, and it can be removed by any spelling.
TEST(LedgerTest, PhrasesAreTheSameHoweverTyped) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);

  EXPECT_THAT(here.AddPhrase("  Buenos Dias "), IsOkAndHolds(true));
  EXPECT_THAT(here.AddPhrase("buenos dias"), IsOkAndHolds(false));
  EXPECT_THAT(here.AddPhrase("GM"), IsOkAndHolds(false));  // A default.

  EXPECT_THAT(here.RemovePhrase("BUENOS DIAS"), IsOkAndHolds(true));
  EXPECT_THAT(here.RemovePhrase("buenos dias"), IsOkAndHolds(false));
}

// What cannot be a phrase is refused with the reason; and since it cannot be
// on the list, removing it is simply "it was not there".
TEST(LedgerTest, RefusesUnacceptablePhrases) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);

  EXPECT_THAT(here.AddPhrase("   "),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(here.AddPhrase("good\nmorning"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(here.RemovePhrase(""), IsOkAndHolds(false));

  EXPECT_THAT(here.Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "morning")));
}

// Even the last phrase can go, leaving nothing that counts.
TEST(LedgerTest, EveryPhraseCanBeRemoved) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  for (const char* const phrase : {"gm", "good morning", "morning"}) {
    EXPECT_THAT(here.RemovePhrase(phrase), IsOkAndHolds(true));
  }

  EXPECT_THAT(here.Phrases(), IsOkAndHolds(testing::IsEmpty()));
}

// A ledger file from before forfeits existed is brought up to date when it is
// opened, with everything in it intact.
TEST(LedgerTest, UpgradesAFileFromBeforeForfeits) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "old.db";
  {
    // The first layout: no lives, and no number saying which layout it is.
    absl::StatusOr<sqlite::Database> old = sqlite::Database::Open(file);
    ABSL_ASSERT_OK(old);
    ABSL_ASSERT_OK(old->Execute(
        "CREATE TABLE members (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"));
    ABSL_ASSERT_OK(old->Execute(
        "CREATE TABLE gms (member_id INTEGER NOT NULL REFERENCES members (id),"
        " day TEXT NOT NULL, PRIMARY KEY (member_id, day)) WITHOUT ROWID"));
    ABSL_ASSERT_OK(old->Execute("INSERT INTO members VALUES (1, 'luke')"));
    ABSL_ASSERT_OK(old->Execute(
        "INSERT INTO gms VALUES (1, '2026-10-01'), (1, '2026-10-02')"));
  }

  absl::StatusOr<Ledger> ledger = Ledger::Open(file);
  ABSL_ASSERT_OK(ledger);
  // A file that old knew nothing of communities; what is in it goes to
  // whichever claims it.
  ABSL_ASSERT_OK(ledger->ClaimUndivided(kHere));

  // The old GMs are there, and the new features work on them.
  const Receipt third = Gm(ledger->community(kHere), kLuke, October(3));
  EXPECT_EQ(third.standing.streak, 3);
  EXPECT_THAT(ledger->community(kHere).Forfeit(kLuke.id, October(3)),
              IsOkAndHolds(3));
  // The phrases that were built in then are the list it starts with.
  EXPECT_THAT(ledger->community(kHere).Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "morning")));
}

// The same goes for a file from after forfeits but before the phrase list,
// and the phrases changed since are kept when it is opened again.
TEST(LedgerTest, UpgradesAFileFromBeforePhrasesAndKeepsLaterChanges) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "lives.db";
  {
    absl::StatusOr<sqlite::Database> old = sqlite::Database::Open(file);
    ABSL_ASSERT_OK(old);
    ABSL_ASSERT_OK(old->Execute(
        "CREATE TABLE members (id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
        " life INTEGER NOT NULL DEFAULT 0)"));
    ABSL_ASSERT_OK(old->Execute(
        "CREATE TABLE gms (member_id INTEGER NOT NULL REFERENCES members (id),"
        " life INTEGER NOT NULL, day TEXT NOT NULL,"
        " PRIMARY KEY (member_id, life, day)) WITHOUT ROWID"));
    ABSL_ASSERT_OK(old->Execute("INSERT INTO members VALUES (1, 'luke', 1)"));
    ABSL_ASSERT_OK(old->Execute("INSERT INTO gms VALUES (1, 0, '2026-10-01')"));
    ABSL_ASSERT_OK(old->Execute("PRAGMA user_version = 2"));
  }
  {
    absl::StatusOr<Ledger> ledger = Ledger::Open(file);
    ABSL_ASSERT_OK(ledger);
    ABSL_ASSERT_OK(ledger->ClaimUndivided(kHere));
    EXPECT_THAT(ledger->community(kHere).Phrases(),
                IsOkAndHolds(ElementsAre("gm", "good morning", "morning")));
    ABSL_ASSERT_OK(ledger->community(kHere).AddPhrase("yo"));

    // Luke's forfeit, from before the upgrade, still stands.
    EXPECT_EQ(Gm(ledger->community(kHere), kLuke, October(2)).standing.streak,
              1);
  }

  absl::StatusOr<Ledger> reopened = Ledger::Open(file);
  ABSL_ASSERT_OK(reopened);
  EXPECT_THAT(reopened->community(kHere).Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "morning", "yo")));
}

// What an old file holds is claimed once, by the first community to ask.
// After that there is nothing left to claim, and a community the ledger
// already knows is never handed someone else's past.
TEST(LedgerTest, UndividedDataIsClaimedOnce) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "claim.db";
  {
    absl::StatusOr<sqlite::Database> old = sqlite::Database::Open(file);
    ABSL_ASSERT_OK(old);
    ABSL_ASSERT_OK(old->Execute(
        "CREATE TABLE members (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"));
    ABSL_ASSERT_OK(old->Execute(
        "CREATE TABLE gms (member_id INTEGER NOT NULL REFERENCES members (id),"
        " day TEXT NOT NULL, PRIMARY KEY (member_id, day)) WITHOUT ROWID"));
    ABSL_ASSERT_OK(old->Execute("INSERT INTO members VALUES (1, 'luke')"));
    ABSL_ASSERT_OK(old->Execute("INSERT INTO gms VALUES (1, '2026-10-01')"));
  }
  absl::StatusOr<Ledger> ledger = Ledger::Open(file);
  ABSL_ASSERT_OK(ledger);
  Community here = ledger->community(kHere);
  Community elsewhere = ledger->community(kElsewhere);

  // Unclaimed, the old GM belongs to nobody.
  const absl::StatusOr<Standing> before = here.StandingOf(kLuke.id, October(1));
  ABSL_ASSERT_OK(before);
  EXPECT_EQ(before->streak, 0);

  ABSL_ASSERT_OK(ledger->ClaimUndivided(kHere));
  ABSL_ASSERT_OK(ledger->ClaimUndivided(kElsewhere));  // Nothing left.

  const absl::StatusOr<Standing> mine = here.StandingOf(kLuke.id, October(1));
  ABSL_ASSERT_OK(mine);
  EXPECT_EQ(mine->streak, 1);
  const absl::StatusOr<Standing> theirs =
      elsewhere.StandingOf(kLuke.id, October(1));
  ABSL_ASSERT_OK(theirs);
  EXPECT_EQ(theirs->streak, 0);
}

// Claiming on a ledger that never had anything undivided, which is every
// start of the bot after the first, changes nothing.
TEST(LedgerTest, ClaimingWithNothingUndividedDoesNothing) {
  Ledger ledger = NewLedger();
  Community here = ledger.community(kHere);
  Gm(here, kLuke, October(1));
  ABSL_ASSERT_OK(here.AddPhrase("yo"));

  ABSL_ASSERT_OK(ledger.ClaimUndivided(kHere));
  ABSL_ASSERT_OK(ledger.ClaimUndivided(kElsewhere));

  EXPECT_EQ(Gm(here, kLuke, October(2)).standing.streak, 2);
  EXPECT_THAT(here.Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "morning", "yo")));
  EXPECT_THAT(ledger.community(kElsewhere).Phrases(),
              IsOkAndHolds(ElementsAre("gm", "good morning", "morning")));
}

// A file from a later version may hold things this one would mishandle, so
// it is refused rather than guessed at.
TEST(LedgerTest, RefusesAFileFromANewerVersion) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "newer.db";
  {
    absl::StatusOr<sqlite::Database> newer = sqlite::Database::Open(file);
    ABSL_ASSERT_OK(newer);
    ABSL_ASSERT_OK(newer->Execute("PRAGMA user_version = 99"));
  }

  EXPECT_THAT(Ledger::Open(file),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //gm:ledger_test -- --benchmark_filter=all
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
//   -------------------------------------------------------------------------------------
//   Benchmark                                           Time             CPU   Iterations
//   -------------------------------------------------------------------------------------
//   BM_Record/10/30/iterations:200                  22736 ns        22733 ns          200
//   BM_Record/50/120/iterations:200                 51198 ns        51195 ns          200
//   BM_RecordAgain/10/30                            17440 ns        17440 ns        23656
//   BM_RecordAgain/50/120                           51516 ns        51486 ns         8035
//   BM_StandingOf/10/30                             12087 ns        12086 ns        34441
//   BM_StandingOf/50/120                            42992 ns        42983 ns         9615
//   BM_Leaderboard/10/30                           105987 ns       105986 ns         3868
//   BM_Leaderboard/50/120                         2028554 ns      2028216 ns          208
//   BM_Phrases                                        969 ns          969 ns       452677
//   BM_ForfeitThenRecord/10/30/iterations:200      101768 ns       101499 ns          200
//   BM_ForfeitThenRecord/50/120/iterations:200     171350 ns       171350 ns          200
// End of results.
// clang-format on

// A community in which each of `members` people has said GM on each of the
// `days` days up to the end of 2025, built once and kept: building it is
// slow, and the benchmark library calls each benchmark several times.
//
// Benchmarks that only read share one such community. One that writes names
// itself as `writer` and gets its own, so that what it adds does not slow
// the others down.
Community Established(int members, int days, std::string_view writer = "") {
  static auto* const ledgers = new std::map<std::tuple<int, int, std::string>,
                                            std::unique_ptr<Ledger>>();
  std::unique_ptr<Ledger>& ledger =
      (*ledgers)[{members, days, std::string(writer)}];
  if (ledger == nullptr) {
    ledger = std::make_unique<Ledger>(NewLedger());
    const Community community = ledger->community(kHere);
    for (int day = 0; day < days; ++day) {
      for (int member = 0; member < members; ++member) {
        Gm(community,
           Member{
               .id = static_cast<uint64_t>(member + 1),
               .name = "member",
           },
           absl::CivilDay(2025, 12, 31) - (days - 1 - day));
      }
    }
  }
  return ledger->community(kHere);
}

// The first argument is how many members the community has and the second
// how many days of history each, so the pairs are a small young server and
// a busy established one.
void Sizes(benchmark::internal::Benchmark* benchmark) {
  benchmark->Args({10, 30})->Args({50, 120});
}

// A fixed number of iterations, for benchmarks that add to the ledger: enough
// to time, and few enough that what they add is small beside what was there.
constexpr int kWrites = 200;

// Recording a GM that counts: the bot's main business. Each one is the next
// day's for one member in turn.
void BM_Record(benchmark::State& state) {
  const int members = static_cast<int>(state.range(0));
  Community community =
      Established(members, static_cast<int>(state.range(1)), "BM_Record");
  // Carries on from where the last call left off, so that every GM is new.
  static auto* const next_day =
      new std::map<std::pair<int64_t, int64_t>, absl::CivilDay>();
  absl::CivilDay& day =
      next_day->try_emplace({state.range(0), state.range(1)}, 2026, 1, 1)
          .first->second;
  int member = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.Record(
        Member{
            .id = static_cast<uint64_t>(member + 1),
            .name = "member",
        },
        day));
    if (++member == members) {
      member = 0;
      ++day;
    }
  }
}
BENCHMARK(BM_Record)->Apply(Sizes)->Iterations(kWrites);

// Recording one that does not count, because they have said it today.
void BM_RecordAgain(benchmark::State& state) {
  Community community = Established(static_cast<int>(state.range(0)),
                                    static_cast<int>(state.range(1)));
  const Member member{
      .id = 1,
      .name = "member",
  };
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        community.Record(member, absl::CivilDay(2025, 12, 31)));
  }
}
BENCHMARK(BM_RecordAgain)->Apply(Sizes);

// What /streak asks.
void BM_StandingOf(benchmark::State& state) {
  Community community = Established(static_cast<int>(state.range(0)),
                                    static_cast<int>(state.range(1)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        community.StandingOf(1, absl::CivilDay(2025, 12, 31)));
  }
}
BENCHMARK(BM_StandingOf)->Apply(Sizes);

// What /leaderboard asks.
void BM_Leaderboard(benchmark::State& state) {
  Community community = Established(static_cast<int>(state.range(0)),
                                    static_cast<int>(state.range(1)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        community.Leaderboard(absl::CivilDay(2025, 12, 31), 10));
  }
}
BENCHMARK(BM_Leaderboard)->Apply(Sizes);

// What every message in a GM channel asks first: which phrases count.
void BM_Phrases(benchmark::State& state) {
  Ledger ledger = NewLedger();
  Community community = ledger.community(kHere);
  for (auto _ : state) benchmark::DoNotOptimize(community.Phrases());
}
BENCHMARK(BM_Phrases);

// Giving up a streak and starting another, as someone does who chats in the
// GM channel and then says GM.
void BM_ForfeitThenRecord(benchmark::State& state) {
  Community community =
      Established(static_cast<int>(state.range(0)),
                  static_cast<int>(state.range(1)), "BM_ForfeitThenRecord");
  const Member member{
      .id = 1,
      .name = "member",
  };
  const absl::CivilDay today(2025, 12, 31);
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.Forfeit(member.id, today));
    benchmark::DoNotOptimize(community.Record(member, today));
  }
}
BENCHMARK(BM_ForfeitThenRecord)->Apply(Sizes)->Iterations(kWrites);

}  // namespace
}  // namespace gm
