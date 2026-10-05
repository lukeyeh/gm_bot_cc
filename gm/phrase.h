// The phrases that count as saying GM: which ones a new ledger starts with,
// what makes a phrase acceptable, and whether a message says one.

#ifndef GM_PHRASE_H_
#define GM_PHRASE_H_

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace gm {

// What a ledger's list of phrases starts out as.
inline constexpr std::array<std::string_view, 3> kDefaultPhrases = {
    "gm",
    "good morning",
    "morning",
};

// `phrase` in the form phrases are kept and compared in: without the space
// around it, and in lower case, so that "  GM " and "gm" are one phrase.
//
// Fails with InvalidArgument if it is not acceptable as a phrase: empty,
// longer than 50 bytes, or containing a line break, another control
// character or a backtick. The error's message is a sentence fit to show to
// whoever proposed the phrase.
absl::StatusOr<std::string> CanonicalPhrase(std::string_view phrase);

// Whether `message` is someone saying one of `phrases`, each in canonical
// form: in any capitalisation, on its own or at the start or end of a longer
// message ("gm everyone!", "ok, good morning"), and as words of its own
// ("gmail" does not say "gm").
bool IsGm(std::string_view message, std::span<const std::string> phrases);

}  // namespace gm

#endif  // GM_PHRASE_H_
