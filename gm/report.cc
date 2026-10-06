#include "gm/report.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "gm/badge.h"
#include "gm/ledger.h"
#include "gm/phrase.h"

namespace gm {
namespace {

constexpr std::array<std::string_view, 3> kMedals = {
    "🥇",
    "🥈",
    "🥉",
};

// "1 day", "2 days".
std::string Days(int count) {
  return absl::StrCat(count, count == 1 ? " day" : " days");
}

// `name` with anything Discord would read as formatting made literal, so
// that a name like "_luke_" appears as written.
std::string Literal(std::string_view name) {
  std::string literal;
  for (const char c : name) {
    if (absl::StrContains("\\*_~`|>", c)) literal.push_back('\\');
    literal.push_back(c);
  }

  return literal;
}

// A line showing the badges a best streak of `best` days holds, or nothing
// if it holds none.
std::string BadgeLine(int best) {
  const std::span<const Badge> earned = BadgesEarnedBy(best);
  if (earned.empty()) return "";

  std::string line = "\nBadges:";
  for (const Badge& badge : earned) absl::StrAppend(&line, " ", badge.emoji);

  return line;
}

// What marks place `place`, counting from 1: a medal, or the number.
std::string Marker(size_t place) {
  if (place <= kMedals.size()) return std::string(kMedals[place - 1]);

  return absl::StrCat("**", place, ".**");
}

}  // namespace

std::string LeaderboardReport(std::span<const Standing> board) {
  std::string report = "🌅 **GM Streak Leaderboard**\n";
  if (board.empty()) {
    absl::StrAppend(&report, "\nNo one has said GM yet. Be the first!");
    return report;
  }

  size_t place = 0;
  for (size_t i = 0; i < board.size(); ++i) {
    // A new place only when the streak differs from the one above.
    if (i == 0 || board[i].streak != board[i - 1].streak) ++place;

    const Standing& standing = board[i];
    absl::StrAppend(&report, "\n", Marker(place), " **",
                    Literal(standing.member.name), "** — ",
                    Days(standing.streak));
    if (standing.best > standing.streak) {
      absl::StrAppend(&report, " (best: ", standing.best, ")");
    }
  }

  return report;
}

std::string PhraseListReport(std::span<const Phrase> phrases) {
  std::string report = "✅ **GM phrases**\n";
  if (phrases.empty()) {
    absl::StrAppend(&report,
                    "\nThere are none, so nothing counts as a GM. An admin "
                    "can add one with `/gmadd`.");
    return report;
  }

  bool any_limited = false;
  for (const Phrase& phrase : phrases) {
    absl::StrAppend(&report, "\n• `", phrase.text, "`");
    if (phrase.hours.all_day()) continue;

    absl::StrAppend(&report, " — ⏰ ", phrase.hours.Describe());
    any_limited = true;
  }
  absl::StrAppend(&report, "\n\nCapitalisation does not matter.");
  if (any_limited) absl::StrAppend(&report, " Hours are on a 24-hour clock.");

  return report;
}

std::string PhraseAddedReport(const Phrase& phrase, bool added) {
  if (!added) {
    return absl::StrCat("`", phrase.text, "` already counts as a GM.");
  }
  if (phrase.hours.all_day()) {
    return absl::StrCat("✅ `", phrase.text, "` now counts as a GM.");
  }

  return absl::StrCat("✅ `", phrase.text, "` now counts as a GM from ",
                      phrase.hours.Describe(), ".");
}

std::string PhraseRemovedReport(std::string_view phrase, bool removed) {
  if (!removed) return absl::StrCat("❌ `", phrase, "` is not a GM phrase.");

  return absl::StrCat("✅ `", phrase, "` no longer counts as a GM.");
}

std::string StreakReport(const Standing& standing, std::string_view mention) {
  if (standing.streak == 0) {
    std::string report = absl::StrCat(
        mention, ", you don't have an active streak. Say GM to start one! 🌅");
    if (standing.best > 0) {
      absl::StrAppend(&report, "\nYour best: **", Days(standing.best), "**");
    }
    absl::StrAppend(&report, BadgeLine(standing.best));

    return report;
  }

  std::string report =
      absl::StrCat("🔥 ", mention, ", your current streak is **",
                   Days(standing.streak), "**!");
  if (standing.best > standing.streak) {
    absl::StrAppend(&report, "\nYour best: **", Days(standing.best), "**");
  }
  absl::StrAppend(&report, BadgeLine(standing.best));

  return report;
}

std::string BadgesReport(const Standing& standing) {
  const size_t earned = BadgesEarnedBy(standing.best).size();
  const std::span<const Badge> all = AllBadges();

  std::string report =
      absl::StrCat("🏅 **GM Streak Badges**\nYour best streak: **",
                   Days(standing.best), "**\n");
  for (size_t i = 0; i < all.size(); ++i) {
    const Badge& badge = all[i];

    // The easiest `earned` of them are the ones held.
    if (i < earned) {
      absl::StrAppend(&report, "\n", badge.emoji, " **", badge.name, "** — ",
                      badge.days, " days ✅");
    } else {
      absl::StrAppend(&report, "\n🔒 ~~", badge.name, "~~ — ", badge.days,
                      " days");
    }
  }
  absl::StrAppend(&report, "\n\nEarned: ", earned, "/", all.size(), " badges");

  return report;
}

}  // namespace gm
