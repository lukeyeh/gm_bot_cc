// The bot's answers to questions about streaks, as text for Discord, which
// reads **this** as bold.

#ifndef GM_REPORT_H_
#define GM_REPORT_H_

#include <span>
#include <string>
#include <string_view>

#include "gm/ledger.h"

namespace gm {

// The leaderboard, given the standings in the order the ledger ranks them.
// Members level on streak share a place, and the first three places get
// medals.
std::string LeaderboardReport(std::span<const Standing> board);

// One member's streak, addressed to them as `mention`.
std::string StreakReport(const Standing& standing, std::string_view mention);

// The phrases that count as a GM, as a list to read.
std::string PhraseListReport(std::span<const std::string> phrases);

// The outcome of someone trying to add `phrase` to the list: `added` is
// whether it was new.
std::string PhraseAddedReport(std::string_view phrase, bool added);

// The outcome of someone trying to remove `phrase` from the list: `removed`
// is whether it was there.
std::string PhraseRemovedReport(std::string_view phrase, bool removed);

}  // namespace gm

#endif  // GM_REPORT_H_
