#include "gm/ledger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "gm/phrase.h"
#include "sqlite/database.h"

namespace gm {
namespace {

// The layout of the database is numbered, and the number kept in the file
// (SQLite's user_version), so that a file written by an older version of this
// code can be brought up to date when it is opened. A new file starts at
// layout 1 and is brought up to date the same way, so there is one path to
// the current layout rather than two.
//
//   1  members (id, name); gms (member_id, day). Files of this layout were
//      not numbered, so they say 0, as an empty file does.
//   2  Adds lives: members.life and gms.life.
//   3  Adds phrases.
//   4  Adds communities: every table gains a community column, and
//      communities lists the ones that have been given their phrases.
constexpr int64_t kLayout = 4;

// What was recorded before layout 4 belongs to no community until one claims
// it. No real community has this id.
constexpr int64_t kUndivided = 0;

// The statements below are each written over several lines, as adjacent
// string literals that the compiler joins, in lists of statements. That is
// just what a forgotten comma between two list items looks like, which is
// what this check is for, so it is off for these lists.
// NOLINTBEGIN(bugprone-suspicious-missing-comma)

// Runs each of `statements` in turn.
absl::Status ExecuteAll(sqlite::Database& database,
                        std::initializer_list<std::string_view> statements) {
  for (const std::string_view statement : statements) {
    ABSL_RETURN_IF_ERROR(database.Execute(statement));
  }
  return absl::OkStatus();
}

// Layout 1. Days are stored as ISO dates ("2026-10-05") so that the database
// can be read by eye, and member ids as the integers they are.
absl::Status CreateFirstLayout(sqlite::Database& database) {
  return ExecuteAll(
      database,
      {
          "CREATE TABLE members (id INTEGER PRIMARY KEY, name TEXT NOT NULL)",
          "CREATE TABLE gms ("
          "  member_id INTEGER NOT NULL REFERENCES members (id),"
          "  day TEXT NOT NULL,"
          "  PRIMARY KEY (member_id, day)) WITHOUT ROWID",
      });
}

// Layout 1 to 2. A member's GMs are divided into lives. Forfeiting a streak
// ends one life and begins the next, and a streak is a run of days within a
// single life: that is how a forfeit breaks a streak without any GM being
// forgotten. `members.life` is the life a member is on, and `gms.life` the
// one each GM was said in. Every GM so far was said in a first life.
absl::Status AddLives(sqlite::Database& database) {
  return ExecuteAll(
      database,
      {
          "ALTER TABLE members ADD COLUMN life INTEGER NOT NULL DEFAULT 0",
          "ALTER TABLE gms RENAME TO gms_1",
          "CREATE TABLE gms ("
          "  member_id INTEGER NOT NULL REFERENCES members (id),"
          "  life INTEGER NOT NULL,"
          "  day TEXT NOT NULL,"
          "  PRIMARY KEY (member_id, life, day)) WITHOUT ROWID",
          "INSERT INTO gms (member_id, life, day)"
          "  SELECT member_id, 0, day FROM gms_1",
          "DROP TABLE gms_1",
      });
}

// Layout 2 to 3. The phrases that count as a GM, each in the canonical form
// of phrase.h, start out as the ones that were built in until then.
absl::Status AddPhrases(sqlite::Database& database) {
  ABSL_RETURN_IF_ERROR(database.Execute(
      "CREATE TABLE phrases (phrase TEXT PRIMARY KEY) WITHOUT ROWID"));

  for (const std::string_view phrase : kDefaultPhrases) {
    ABSL_RETURN_IF_ERROR(database.Execute(
        "INSERT INTO phrases (phrase) VALUES (?1)", {
                                                        std::string(phrase),
                                                    }));
  }
  return absl::OkStatus();
}

// Layout 3 to 4. Everything is divided by community. What is there already
// goes to the "undivided" community, for a real one to claim.
//
// The reference from a GM to its member is checked when a transaction ends
// rather than as each row changes, so that claiming can move the two tables
// one after the other.
absl::Status AddCommunities(sqlite::Database& database) {
  return ExecuteAll(database,
                    {
                        "CREATE TABLE communities (id INTEGER PRIMARY KEY)",

                        "ALTER TABLE members RENAME TO members_3",
                        "CREATE TABLE members ("
                        "  community INTEGER NOT NULL,"
                        "  id INTEGER NOT NULL,"
                        "  name TEXT NOT NULL,"
                        "  life INTEGER NOT NULL DEFAULT 0,"
                        "  PRIMARY KEY (community, id)) WITHOUT ROWID",
                        "INSERT INTO members (community, id, name, life)"
                        "  SELECT 0, id, name, life FROM members_3",

                        "ALTER TABLE gms RENAME TO gms_3",
                        "CREATE TABLE gms ("
                        "  community INTEGER NOT NULL,"
                        "  member_id INTEGER NOT NULL,"
                        "  life INTEGER NOT NULL,"
                        "  day TEXT NOT NULL,"
                        "  PRIMARY KEY (community, member_id, life, day),"
                        "  FOREIGN KEY (community, member_id)"
                        "    REFERENCES members (community, id)"
                        "    DEFERRABLE INITIALLY DEFERRED) WITHOUT ROWID",
                        "INSERT INTO gms (community, member_id, life, day)"
                        "  SELECT 0, member_id, life, day FROM gms_3",

                        "ALTER TABLE phrases RENAME TO phrases_3",
                        "CREATE TABLE phrases ("
                        "  community INTEGER NOT NULL,"
                        "  phrase TEXT NOT NULL,"
                        "  PRIMARY KEY (community, phrase)) WITHOUT ROWID",
                        "INSERT INTO phrases (community, phrase)"
                        "  SELECT 0, phrase FROM phrases_3",

                        "DROP TABLE gms_3",
                        "DROP TABLE members_3",
                        "DROP TABLE phrases_3",
                    });
}

// A standing, plus what is needed to tell whether a record was just broken.
struct Tally {
  Standing standing;
  // The longest of the member's streaks other than the live one.
  int earlier_best = 0;
};

// One GM, as the tallying below wants it: whose, in which of their lives,
// and how many days before today (so never positive).
struct Said {
  int64_t member_id = 0;
  int64_t life = 0;
  int64_t day = 0;
};

// Works out one member's tally from their GMs, which are in order of life
// and then of day.
//
// A streak is a run of consecutive days within one life. The live one, if
// there is one, is the last run of the life the member is on, provided it
// reaches today or yesterday.
Tally TallyOf(Member member, int64_t current_life, std::span<const Said> gms) {
  int streak = 0;
  int earlier_best = 0;

  size_t start = 0;
  while (start < gms.size()) {
    // The run that begins at `start` goes on for as long as each GM is in
    // the same life as the one before it and on the next day.
    size_t end = start + 1;
    while (end < gms.size() && gms[end].life == gms[end - 1].life &&
           gms[end].day == gms[end - 1].day + 1) {
      ++end;
    }

    const int length = static_cast<int>(end - start);
    const Said& last = gms[end - 1];
    if (last.life == current_life && last.day >= -1) {
      streak = length;
    } else {
      earlier_best = std::max(earlier_best, length);
    }
    start = end;
  }

  return Tally{
      .standing =
          Standing{
              .member = std::move(member),
              .streak = streak,
              .best = std::max(streak, earlier_best),
          },
      .earlier_best = earlier_best,
  };
}

// NOLINTEND(bugprone-suspicious-missing-comma)

// The members of a community, or one of them: id, name and the life they are
// on, in order of id.
constexpr std::string_view kMembersOfCommunity =
    "SELECT id, name, life FROM members WHERE community = ?1 ORDER BY id";
constexpr std::string_view kMemberOfCommunity =
    "SELECT id, name, life FROM members WHERE community = ?1 AND id = ?2";

// The GMs of a community, or of one member of it, up to a day: whose, in
// which life, and on which day. In order of member, then life, then day,
// which is the order of the table's key.
//
// ISO dates compare as text the way they compare as dates, which is how later
// days are left out.
constexpr std::string_view kGmsOfCommunity =
    "SELECT member_id, life, day FROM gms "
    "WHERE community = ?1 AND day <= ?2 "
    "ORDER BY member_id, life, day";
constexpr std::string_view kGmsOfMember =
    "SELECT member_id, life, day FROM gms "
    "WHERE community = ?1 AND member_id = ?2 AND day <= ?3 "
    "ORDER BY life, day";

// The day that `text`, a date as the ledger stores them ("2026-10-05"),
// names. The ledger writes every one of them itself, with FormatCivilTime, so
// there is no other form to allow for.
absl::CivilDay StoredDay(std::string_view text) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (text.size() == 10) {
    (void)absl::SimpleAtoi(text.substr(0, 4), &year);
    (void)absl::SimpleAtoi(text.substr(5, 2), &month);
    (void)absl::SimpleAtoi(text.substr(8, 2), &day);
  }
  return absl::CivilDay(year, month, day);
}

// The tallies, as of `today`, of everyone in `community` who had said GM by
// then, or of just the member with `member_id`. In no particular order.
//
// Days after today are left out: as of today they have not happened.
absl::StatusOr<std::vector<Tally>> Tallies(
    sqlite::Database& database, const int64_t community,
    const absl::CivilDay today, const std::optional<uint64_t> member_id) {
  const std::string day = absl::FormatCivilTime(today);

  // Naming the member in the query, where there is one, lets SQLite go
  // straight to that member's rows instead of reading the community's.
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> member_rows,
      member_id.has_value()
          ? database.Query(kMemberOfCommunity,
                           {
                               community,
                               static_cast<int64_t>(*member_id),
                           })
          : database.Query(kMembersOfCommunity, {
                                                    community,
                                                }));
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> gm_rows,
      member_id.has_value()
          ? database.Query(kGmsOfMember,
                           {
                               community,
                               static_cast<int64_t>(*member_id),
                               day,
                           })
          : database.Query(kGmsOfCommunity, {
                                                community,
                                                day,
                                            }));

  std::vector<Said> gms;
  gms.reserve(gm_rows.size());
  for (const sqlite::Row& row : gm_rows) {
    gms.push_back(Said{
        .member_id = row.Int(0),
        .life = row.Int(1),
        .day = StoredDay(row.Text(2)) - today,
    });
  }

  std::vector<Tally> tallies;
  size_t next = 0;
  for (const sqlite::Row& row : member_rows) {
    // This member's GMs are the next stretch of the list.
    const size_t first = next;
    while (next < gms.size() && gms[next].member_id == row.Int(0)) ++next;
    if (next == first) continue;  // None yet, as of today.

    tallies.push_back(TallyOf(
        Member{
            .id = static_cast<uint64_t>(row.Int(0)),
            .name = std::string(row.Text(1)),
        },
        row.Int(2), std::span<const Said>(gms).subspan(first, next - first)));
  }
  return tallies;
}

absl::StatusOr<std::vector<std::string>> ReadPhrases(sqlite::Database& database,
                                                     const int64_t community) {
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> rows,
      database.Query(
          "SELECT phrase FROM phrases WHERE community = ?1 ORDER BY phrase",
          {
              community,
          }));

  std::vector<std::string> phrases;
  phrases.reserve(rows.size());
  for (const sqlite::Row& row : rows) {
    phrases.emplace_back(row.Text(0));
  }
  return phrases;
}

// The one tally that asking about a single member yields, if any.
absl::StatusOr<std::optional<Tally>> TallyOfMember(sqlite::Database& database,
                                                   const int64_t community,
                                                   const absl::CivilDay today,
                                                   const uint64_t member_id) {
  ABSL_ASSIGN_OR_RETURN(std::vector<Tally> tallies,
                        Tallies(database, community, today, member_id));
  if (tallies.empty()) return std::nullopt;

  return std::move(tallies.front());
}

}  // namespace

absl::StatusOr<Ledger> Ledger::Prepare(sqlite::Database database) {
  ABSL_ASSIGN_OR_RETURN(const std::vector<sqlite::Row> version,
                        database.Query("PRAGMA user_version"));
  int64_t layout = version.empty() ? 0 : version.front().Int(0);
  if (layout > kLayout) {
    return absl::FailedPreconditionError(
        "the ledger was written by a newer version of this program");
  }
  if (layout == kLayout) return Ledger(std::move(database));

  // One step at a time from whatever layout the file has to the current one.
  ABSL_RETURN_IF_ERROR(database.Transaction([&database,
                                             &layout]() -> absl::Status {
    if (layout == 0) {
      // An unnumbered file is either new and empty, or of layout 1.
      ABSL_ASSIGN_OR_RETURN(
          const std::vector<sqlite::Row> existing,
          database.Query("SELECT 1 FROM sqlite_schema WHERE name = 'members'"));
      if (existing.empty()) {
        ABSL_RETURN_IF_ERROR(CreateFirstLayout(database));
      }
      layout = 1;
    }

    if (layout == 1) {
      ABSL_RETURN_IF_ERROR(AddLives(database));
      layout = 2;
    }
    if (layout == 2) {
      ABSL_RETURN_IF_ERROR(AddPhrases(database));
      layout = 3;
    }
    if (layout == 3) {
      ABSL_RETURN_IF_ERROR(AddCommunities(database));
      layout = 4;
    }

    return database.Execute(absl::StrCat("PRAGMA user_version = ", kLayout));
  }));

  return Ledger(std::move(database));
}

absl::StatusOr<Ledger> Ledger::Open(const std::filesystem::path& file) {
  ABSL_ASSIGN_OR_RETURN(sqlite::Database database,
                        sqlite::Database::Open(file));
  return Prepare(std::move(database));
}

absl::StatusOr<Ledger> Ledger::InMemory() {
  ABSL_ASSIGN_OR_RETURN(sqlite::Database database,
                        sqlite::Database::InMemory());
  return Prepare(std::move(database));
}

absl::Status Ledger::ClaimUndivided(const uint64_t community) {
  const int64_t id = static_cast<int64_t>(community);
  sqlite::Database& database = *database_;

  return database.Transaction([&database, id]() -> absl::Status {
    // Enrolling fails to add a row if the community is already known, in
    // which case it has phrases of its own and perhaps members, and what is
    // undivided stays that way.
    ABSL_ASSIGN_OR_RETURN(
        const std::vector<sqlite::Row> newly_known,
        database.Query("INSERT INTO communities (id) "
                       "SELECT ?1 WHERE EXISTS "
                       "  (SELECT 1 FROM phrases WHERE community = ?2 UNION ALL"
                       "   SELECT 1 FROM members WHERE community = ?2) "
                       "ON CONFLICT DO NOTHING RETURNING 1",
                       {
                           id,
                           kUndivided,
                       }));
    if (newly_known.empty()) return absl::OkStatus();

    for (const std::string_view table : {"members", "gms", "phrases"}) {
      ABSL_RETURN_IF_ERROR(database.Execute(
          absl::StrCat("UPDATE ", table,
                       " SET community = ?1 WHERE community = ?2"),
          {
              id,
              kUndivided,
          }));
    }
    return absl::OkStatus();
  });
}

absl::Status Community::Enrol() {
  // RETURNING yields a row only if the community was new.
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> newly_known,
      database_->Query("INSERT INTO communities (id) VALUES (?1) "
                       "ON CONFLICT DO NOTHING RETURNING 1",
                       {
                           id_,
                       }));
  if (newly_known.empty()) return absl::OkStatus();

  for (const std::string_view phrase : kDefaultPhrases) {
    ABSL_RETURN_IF_ERROR(database_->Execute(
        "INSERT INTO phrases (community, phrase) VALUES (?1, ?2)",
        {
            id_,
            std::string(phrase),
        }));
  }
  return absl::OkStatus();
}

absl::StatusOr<Receipt> Community::Record(const Member& member,
                                          const absl::CivilDay day) {
  const int64_t member_id = static_cast<int64_t>(member.id);
  Receipt receipt;
  ABSL_RETURN_IF_ERROR(database_->Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(database_->Execute(
        // Rewritten only if the name has changed, so that a GM from someone
        // known, which is nearly every GM, leaves the table untouched.
        "INSERT INTO members (community, id, name) VALUES (?1, ?2, ?3) "
        "ON CONFLICT (community, id) DO UPDATE SET name = excluded.name "
        "WHERE name <> excluded.name",
        {
            id_,
            member_id,
            member.name,
        }));

    // Said in the life the member is on. RETURNING yields a row only if the
    // GM was new.
    ABSL_ASSIGN_OR_RETURN(
        const std::vector<sqlite::Row> inserted,
        database_->Query("INSERT INTO gms (community, member_id, life, day) "
                         "SELECT community, id, life, ?3 FROM members "
                         "WHERE community = ?1 AND id = ?2 "
                         "ON CONFLICT DO NOTHING RETURNING 1",
                         {
                             id_,
                             member_id,
                             absl::FormatCivilTime(day),
                         }));

    ABSL_ASSIGN_OR_RETURN(const std::optional<Tally> tally,
                          TallyOfMember(*database_, id_, day, member.id));
    if (!tally.has_value()) {
      return absl::InternalError("a GM was recorded but cannot be found");
    }

    receipt = Receipt{
        .counted = !inserted.empty(),
        .new_record = !inserted.empty() && tally->earlier_best > 0 &&
                      tally->standing.streak == tally->earlier_best + 1,
        .standing = tally->standing,
    };
    return absl::OkStatus();
  }));

  return receipt;
}

absl::StatusOr<Standing> Community::StandingOf(const uint64_t member_id,
                                               const absl::CivilDay today) {
  ABSL_ASSIGN_OR_RETURN(const std::optional<Tally> tally,
                        TallyOfMember(*database_, id_, today, member_id));

  // Never having said GM is a standing like any other: zero.
  if (!tally.has_value()) {
    return Standing{
        .member =
            Member{
                .id = member_id,
                .name = "",
            },
    };
  }

  return tally->standing;
}

absl::StatusOr<std::vector<std::string>> Community::Phrases() {
  ABSL_ASSIGN_OR_RETURN(std::vector<std::string> phrases,
                        ReadPhrases(*database_, id_));
  if (!phrases.empty()) return phrases;

  // None, which is either because every one has been removed or because the
  // ledger has not heard of this community before. In that case it is now
  // given its starting phrases. Asking only when there are none keeps this
  // off the path of every message in a GM channel.
  ABSL_RETURN_IF_ERROR(Enrol());
  return ReadPhrases(*database_, id_);
}

absl::StatusOr<bool> Community::AddPhrase(const std::string_view phrase) {
  ABSL_ASSIGN_OR_RETURN(const std::string canonical, CanonicalPhrase(phrase));
  ABSL_RETURN_IF_ERROR(Enrol());

  // RETURNING yields a row only if the phrase was new.
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> inserted,
      database_->Query(
          "INSERT INTO phrases (community, phrase) VALUES (?1, ?2) "
          "ON CONFLICT DO NOTHING RETURNING 1",
          {
              id_,
              canonical,
          }));
  return !inserted.empty();
}

absl::StatusOr<bool> Community::RemovePhrase(const std::string_view phrase) {
  // What could not be a phrase cannot be on the list.
  const absl::StatusOr<std::string> canonical = CanonicalPhrase(phrase);
  if (!canonical.ok()) return false;
  ABSL_RETURN_IF_ERROR(Enrol());

  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> deleted,
      database_->Query("DELETE FROM phrases WHERE community = ?1 "
                       "AND phrase = ?2 RETURNING 1",
                       {
                           id_,
                           *canonical,
                       }));
  return !deleted.empty();
}

absl::StatusOr<int> Community::Forfeit(const uint64_t member_id,
                                       const absl::CivilDay today) {
  int forfeited = 0;
  ABSL_RETURN_IF_ERROR(database_->Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(const std::optional<Tally> tally,
                          TallyOfMember(*database_, id_, today, member_id));

    // With no live streak there is nothing to lose.
    if (!tally.has_value() || tally->standing.streak == 0) {
      return absl::OkStatus();
    }

    forfeited = tally->standing.streak;
    return database_->Execute(
        "UPDATE members SET life = life + 1 WHERE community = ?1 AND id = ?2",
        {
            id_,
            static_cast<int64_t>(member_id),
        });
  }));

  return forfeited;
}

absl::StatusOr<std::vector<Standing>> Community::Leaderboard(
    const absl::CivilDay today, const int limit) {
  ABSL_ASSIGN_OR_RETURN(std::vector<Tally> tallies,
                        Tallies(*database_, id_, today, std::nullopt));

  // Longest live streak first, then best streak, then name.
  std::ranges::sort(tallies, [](const Tally& a, const Tally& b) {
    return std::tie(b.standing.streak, b.standing.best,
                    a.standing.member.name) <
           std::tie(a.standing.streak, a.standing.best, b.standing.member.name);
  });

  std::vector<Standing> standings;
  standings.reserve(std::min<size_t>(tallies.size(), std::max(limit, 0)));
  for (Tally& tally : tallies) {
    if (standings.size() == standings.capacity()) break;
    standings.push_back(std::move(tally.standing));
  }
  return standings;
}

}  // namespace gm
