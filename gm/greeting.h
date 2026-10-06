// The bot's manners: what, if anything, is worth saying back to a GM, and
// what to say to something that was not one.

#ifndef GM_GREETING_H_
#define GM_GREETING_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "absl/time/civil_time.h"
#include "gm/ledger.h"
#include "gm/phrase.h"

namespace gm {

// The emoji a GM is acknowledged with.
inline constexpr std::string_view kAcknowledgement = "🌅";

// The emoji that marks a message that had no business in the GM channel.
inline constexpr std::string_view kDisapproval = "👎";

// The emoji that marks a GM said at an hour when its phrase does not count.
inline constexpr std::string_view kOutOfHours = "⏰";

// What to tell someone, addressed as `mention`, who wrote something other
// than a GM in the GM channel, and who forfeited a streak of `forfeited` days
// by it (0 if they had none to lose). `phrases` are what would have counted.
std::string Rebuke(int forfeited, std::string_view mention,
                   std::span<const Phrase> phrases);

// What to tell someone, addressed as `mention`, who said `phrase` at an hour
// when it does not count: when it does, and what time it is `now` in
// `time_zone`, the name of the zone the bot keeps time in.
std::string OutOfHoursNotice(const Phrase& phrase, std::string_view mention,
                             absl::CivilMinute now, std::string_view time_zone);

// What to announce after a GM with this `receipt`, addressing its author as
// `mention`. Most days there is nothing to say: only winning a badge,
// starting a streak, beating a record, a milestone, or a repeated GM gets a
// message. When a GM is more than one of those, the first in that list is
// what is announced.
std::optional<std::string> Announcement(const Receipt& receipt,
                                        std::string_view mention);

}  // namespace gm

#endif  // GM_GREETING_H_
