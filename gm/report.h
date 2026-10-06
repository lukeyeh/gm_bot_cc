// The bot's answers to questions about streaks, as text for Discord, which
// reads **this** as bold.

#ifndef GM_REPORT_H_
#define GM_REPORT_H_

#include <span>
#include <string>
#include <string_view>

#include "gm/ledger.h"
#include "gm/phrase.h"

namespace gm {

// The leaderboard, given the standings in the order the ledger ranks them.
// Members level on streak share a place, and the first three places get
// medals.
std::string LeaderboardReport(std::span<const Standing> board);

// One member's streak, addressed to them as `mention`, with the badges they
// hold if they hold any.
std::string StreakReport(const Standing& standing, std::string_view mention);

// Every badge there is, showing which of them the member with this
// `standing` holds and what the rest would take.
std::string BadgesReport(const Standing& standing);

// The phrases that count as a GM, as a list to read, with the hours of any
// that do not count all day.
std::string PhraseListReport(std::span<const Phrase> phrases);

// The outcome of someone trying to add `phrase` to the list: `added` is
// whether it was new.
std::string PhraseAddedReport(const Phrase& phrase, bool added);

// The outcome of someone trying to remove `phrase` from the list: `removed`
// is whether it was there.
std::string PhraseRemovedReport(std::string_view phrase, bool removed);

}  // namespace gm

#endif  // GM_REPORT_H_
