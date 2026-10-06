// The record of who said GM on which day, kept in a SQLite database, and the
// streaks that follow from it.
//
// The ledger stores only the GMs themselves. Streaks are worked out from them
// when asked for, so they are always consistent with the record: a streak
// ends by a day passing without a GM, not by anything being updated.
//
// A streak is a run of consecutive days with a GM. It is still alive on the
// day after its last GM, since there is still time to extend it, and is over
// once a whole day has been missed.
//
// A streak can also be forfeited, which ends it there and then. The GMs that
// made it up are kept, and it still counts towards the member's best.
//
// A ledger is divided into communities, each with its own members, streaks
// and list of the phrases that count as saying GM. That list starts as
// kDefaultPhrases and can be added to and removed from.
//
// Every question is asked "as of" a day, and answered from the GMs up to and
// including that day. GMs recorded for later days, which a clock set back or
// a change of time zone can leave behind, do not count until their day comes.

#ifndef GM_LEDGER_H_
#define GM_LEDGER_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "gm/phrase.h"
#include "sqlite/database.h"

namespace gm {

// Someone who says GM. `id` is what identifies them; `name` is what to call
// them, and may change from one GM to the next.
struct Member {
  uint64_t id = 0;
  std::string name;
};

// How a member is doing as of some day.
struct Standing {
  Member member;
  // The length in days of their live streak, or 0 if they have none.
  int streak = 0;
  // The length of their longest streak ever, the live one included.
  int best = 0;
};

// What recording a GM did.
struct Receipt {
  // False if the member had already said GM that day, in which case nothing
  // changed.
  bool counted = false;
  // True if this GM is the one that took the member's streak past the longest
  // they had ever had before it. Never true during a first streak, which has
  // no earlier record to beat.
  bool new_record = false;
  // Their standing as of that day.
  Standing standing;
  // What their best streak was before this GM. Less than `standing.best` if
  // this GM is what raised it, which it does by a day at most; otherwise the
  // same.
  int best_before = 0;
};

// One community's part of a ledger: who in it said GM when, and which
// phrases count there. Communities share nothing: the same person in two of
// them has two streaks.
//
// This is a handle on the ledger it came from, cheap to make and to copy,
// and must not outlive that ledger.
class Community {
 public:
  // Records that `member` said GM on `day`. Saying it again on the same day
  // changes nothing but the name they are known by.
  absl::StatusOr<Receipt> Record(const Member& member, absl::CivilDay day);

  // The phrases that count as a GM, in alphabetical order.
  absl::StatusOr<std::vector<Phrase>> Phrases();

  // Adds `phrase`, in any capitalisation, to the phrases that count, during
  // `hours` of the day. Returns false if it was one already, in which case
  // its hours stay as they were. Fails with InvalidArgument, in words fit to
  // show whoever proposed it, if it is not acceptable as a phrase.
  absl::StatusOr<bool> AddPhrase(std::string_view phrase, Hours hours = {});

  // Removes `phrase`, in any capitalisation, from the phrases that count.
  // Returns false if it was not one. The last phrase can be removed, after
  // which nothing is a GM until one is added.
  absl::StatusOr<bool> RemovePhrase(std::string_view phrase);

  // Ends the live streak, as of `today`, of the member with this id, and
  // returns how long it was: 0 if they had none, in which case nothing
  // changes. Their next GM starts a new streak, even one later the same day.
  //
  // A forfeit is not dated: it holds whatever day is asked about afterwards.
  absl::StatusOr<int> Forfeit(uint64_t member_id, absl::CivilDay today);

  // The standing of the member with this id as of `today`. Someone who has
  // never said GM has a standing of zero and no name.
  absl::StatusOr<Standing> StandingOf(uint64_t member_id, absl::CivilDay today);

  // Up to `limit` members who have ever said GM, as of `today`: longest live
  // streak first, ties broken by best streak and then by name.
  absl::StatusOr<std::vector<Standing>> Leaderboard(absl::CivilDay today,
                                                    int limit);

 private:
  friend class Ledger;

  Community(sqlite::Database* database, uint64_t id)
      : database_(database), id_(static_cast<int64_t>(id)) {}

  // Gives the community its starting phrases, if the ledger has not heard of
  // it before.
  absl::Status Enrol();

  sqlite::Database* database_;
  // As the database stores it.
  int64_t id_;
};

class Ledger {
 public:
  // Opens the ledger kept in `file`, creating it if there is none yet, and
  // bringing it up to date if an earlier version of this code wrote it. Fails
  // with FailedPrecondition if a later version did.
  static absl::StatusOr<Ledger> Open(const std::filesystem::path& file);

  // A ledger that is empty and forgotten when destroyed.
  static absl::StatusOr<Ledger> InMemory();

  // The part of the ledger that belongs to the community with this id, which
  // may be any number but 0. One the ledger has not seen before has no GMs,
  // and kDefaultPhrases for phrases.
  Community community(uint64_t id) { return Community(database_.get(), id); }

  // Gives what was recorded before ledgers were divided into communities to
  // the community with this id, provided the ledger has not seen that
  // community before. Does nothing if there is nothing from then, which is
  // the case for any ledger this has already been done to.
  absl::Status ClaimUndivided(uint64_t community);

 private:
  explicit Ledger(sqlite::Database database)
      : database_(std::make_unique<sqlite::Database>(std::move(database))) {}

  static absl::StatusOr<Ledger> Prepare(sqlite::Database database);

  // On the heap so that communities' handles survive the ledger being moved.
  std::unique_ptr<sqlite::Database> database_;
};

}  // namespace gm

#endif  // GM_LEDGER_H_
