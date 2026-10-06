#include "gm/greeting.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/civil_time.h"
#include "gm/badge.h"
#include "gm/ledger.h"
#include "gm/phrase.h"

namespace gm {
namespace {

// The most phrases worth spelling out in a rebuke.
constexpr size_t kMaxPhrasesListed = 5;

// Streak lengths that get a fanfare of their own.
constexpr std::array<int, 4> kMilestones = {
    10,
    25,
    50,
    100,
};

// "1 day", "2 days".
std::string Days(const int count) {
  return absl::StrCat(count, count == 1 ? " day" : " days");
}

}  // namespace

std::string Rebuke(const int forfeited, const std::string_view mention,
                   const std::span<const Phrase> phrases) {
  std::string rebuke =
      absl::StrCat("👎 ", mention, ", only GMs belong in this channel!");
  if (forfeited > 0) {
    absl::StrAppend(&rebuke, "\n**Your ", forfeited,
                    " day streak has been reset to 0.** 💔");
  }

  // A reminder of what does belong: the phrases themselves if they are few,
  // otherwise where to find them.
  if (phrases.empty() || phrases.size() > kMaxPhrasesListed) {
    absl::StrAppend(&rebuke, "\nUse `/gmlist` to see what counts as a GM.");
    return rebuke;
  }

  absl::StrAppend(&rebuke, "\nSay ");
  for (size_t i = 0; i < phrases.size(); ++i) {
    const std::string_view separator =
        i == 0 ? "" : (i + 1 == phrases.size() ? " or " : ", ");
    absl::StrAppend(&rebuke, separator, "`", phrases[i].text, "`");
  }
  absl::StrAppend(&rebuke, " to start a new one.");

  return rebuke;
}

std::string OutOfHoursNotice(const Phrase& phrase,
                             const std::string_view mention,
                             const absl::CivilMinute now,
                             const std::string_view time_zone) {
  return absl::StrCat("⏰ ", mention, ", `", phrase.text,
                      "` only counts from **", phrase.hours.Describe(),
                      "**. It is now ",
                      absl::StrFormat("%d:%02d", now.hour(), now.minute()),
                      " (", time_zone, ").");
}

std::optional<std::string> Announcement(const Receipt& receipt,
                                        const std::string_view mention) {
  const int streak = receipt.standing.streak;

  if (!receipt.counted) {
    return absl::StrCat(
        "☀️ ", mention, ", you already said GM today! Your current streak is **",
        Days(streak), "**. Come back tomorrow to keep it going!");
  }

  if (const Badge* const badge =
          BadgeNewlyEarned(receipt.best_before, receipt.standing.best)) {
    return absl::StrCat(badge->emoji, " **NEW BADGE UNLOCKED!** ", mention,
                        " earned the **", badge->name, "** badge with a **",
                        streak, " day streak!** ", badge->emoji);
  }

  if (receipt.new_record) {
    return absl::StrCat("🔥 **New personal record!** ", mention, " is on a **",
                        streak, " day streak!** Keep it up! 🚀");
  }

  if (streak == 1) {
    return absl::StrCat("Good morning ", mention,
                        "! Your streak has started! ☀️");
  }

  if (streak % 7 == 0) {
    const int weeks = streak / 7;
    return absl::StrCat(
        "🎉 **", streak, " days!** ", mention, " has been saying GM for ",
        weeks, weeks == 1 ? " week" : " weeks", "! Amazing dedication! 💪");
  }

  if (std::ranges::find(kMilestones, streak) != kMilestones.end()) {
    return absl::StrCat("🏆 **MILESTONE!** ", mention, " has reached a **",
                        streak, " day streak!** Legendary! 🌟");
  }

  return std::nullopt;
}

}  // namespace gm
