// The phrases that count as saying GM: which ones a new ledger starts with,
// what makes a phrase acceptable, the hours of the day one can be limited
// to, and what a message amounts to given the phrases there are.

#ifndef GM_PHRASE_H_
#define GM_PHRASE_H_

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace gm {

// The hours of the day during which a phrase counts. By default, all of
// them.
class Hours {
 public:
  Hours() = default;

  // From the start of hour `from` to the start of hour `until`, on a 24-hour
  // clock, running past midnight if `until` is not the later: Between(22, 2)
  // is ten at night to two in the morning. From an hour round to the same
  // hour is the whole day.
  //
  // Fails with InvalidArgument, in words fit to show whoever asked, unless
  // `from` is 0 to 23 and `until` 0 to 24.
  static absl::StatusOr<Hours> Between(int from, int until);

  // Hours as a person writes them: "5-12", or "anytime" for all day. Fails
  // as Between does, and if `text` is neither.
  static absl::StatusOr<Hours> Parse(std::string_view text);

  bool all_day() const { return from_ == until_; }

  // Whether `hour`, 0 to 23, is one of them.
  bool Contains(int hour) const;

  // The hours they run from and until, each 0 to 23. Both are 0 if all day.
  int from() const { return from_; }
  int until() const { return until_; }

  // As shown to people: "5:00 to 12:00", or "any time".
  std::string Describe() const;

  friend bool operator==(const Hours&, const Hours&) = default;

 private:
  int from_ = 0;
  int until_ = 0;
};

// A phrase that counts as a GM, and when.
struct Phrase {
  // In canonical form: see CanonicalPhrase.
  std::string text;
  Hours hours;

  friend bool operator==(const Phrase&, const Phrase&) = default;
};

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

// What a message in a GM channel amounts to.
struct Reading {
  enum class Kind {
    // It says a phrase, at an hour when that phrase counts.
    kGm,
    // It says a phrase, but none that counts at this hour.
    kOutOfHours,
    // It says none.
    kOther,
  };

  Kind kind = Kind::kOther;
  // The phrase it says, one of those given to Read; null for kOther. If it
  // says several, this is the first that counts at this hour, or failing
  // that the first.
  const Phrase* phrase = nullptr;
};

// What `message`, written during `hour` (0 to 23), amounts to given the
// `phrases` there are. A message says a phrase in any capitalisation, with
// the phrase on its own or at the start or end of something longer ("gm
// everyone!", "ok, good morning"), and as words of its own ("gmail" does
// not say "gm").
Reading Read(std::string_view message, std::span<const Phrase> phrases,
             int hour);

}  // namespace gm

#endif  // GM_PHRASE_H_
