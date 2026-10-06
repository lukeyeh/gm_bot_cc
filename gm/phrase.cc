#include "gm/phrase.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"

namespace gm {
namespace {

constexpr size_t kMaxPhraseBytes = 50;

bool IsWordBreak(char c) { return !absl::ascii_isalnum(c); }

// Text as it is compared: trimmed and in lower case.
std::string Folded(std::string_view text) {
  return absl::AsciiStrToLower(absl::StripAsciiWhitespace(text));
}

}  // namespace

absl::StatusOr<std::string> CanonicalPhrase(std::string_view phrase) {
  std::string canonical = Folded(phrase);

  if (canonical.empty()) {
    return absl::InvalidArgumentError("A phrase cannot be empty.");
  }
  if (canonical.size() > kMaxPhraseBytes) {
    return absl::InvalidArgumentError(absl::StrCat(
        "A phrase cannot be longer than ", kMaxPhraseBytes, " characters."));
  }

  // A phrase is shown back to people between backticks, on one line.
  const bool unprintable = std::ranges::any_of(
      canonical, [](char c) { return absl::ascii_iscntrl(c) || c == '`'; });
  if (unprintable) {
    return absl::InvalidArgumentError(
        "A phrase cannot contain line breaks or backticks.");
  }

  return canonical;
}

absl::StatusOr<Hours> Hours::Between(const int from, const int until) {
  if (from < 0 || from > 23 || until < 0 || until > 24) {
    return absl::InvalidArgumentError(
        "Hours run from 0 to 23, as in `5-12` or `22-2`.");
  }

  Hours hours;
  // Midnight is hour 0 whichever end of the day it is written from, and
  // every way of writing the whole day is kept as the same one.
  hours.from_ = from;
  hours.until_ = until % 24;
  if (hours.from_ == hours.until_) return Hours();

  return hours;
}

absl::StatusOr<Hours> Hours::Parse(const std::string_view text) {
  const std::string_view trimmed = absl::StripAsciiWhitespace(text);
  if (absl::EqualsIgnoreCase(trimmed, "anytime")) return Hours();

  const std::pair<std::string_view, std::string_view> ends =
      absl::StrSplit(trimmed, absl::MaxSplits('-', 1));
  int from = 0;
  int until = 0;
  if (!absl::SimpleAtoi(ends.first, &from) ||
      !absl::SimpleAtoi(ends.second, &until)) {
    return absl::InvalidArgumentError(
        "Give the hours as two numbers, like `5-12`, or say `anytime`.");
  }

  return Between(from, until);
}

bool Hours::Contains(const int hour) const {
  if (from_ < until_) return from_ <= hour && hour < until_;

  // Past midnight, or all day.
  return hour >= from_ || hour < until_;
}

std::string Hours::Describe() const {
  if (all_day()) return "any time";

  return absl::StrCat(from_, ":00 to ", until_, ":00");
}

Reading Read(const std::string_view message,
             const std::span<const Phrase> phrases, const int hour) {
  const std::string text = Folded(message);

  // The phrase has to stand as words of its own: "gmail" is not a GM.
  const auto says = [&text](const Phrase& phrase) {
    std::string_view after = text;
    if (absl::ConsumePrefix(&after, phrase.text) &&
        (after.empty() || IsWordBreak(after.front()))) {
      return true;
    }

    std::string_view before = text;
    return absl::ConsumeSuffix(&before, phrase.text) &&
           (before.empty() || IsWordBreak(before.back()));
  };

  const Phrase* out_of_hours = nullptr;
  for (const Phrase& phrase : phrases) {
    if (!says(phrase)) continue;

    if (phrase.hours.Contains(hour)) {
      return Reading{
          .kind = Reading::Kind::kGm,
          .phrase = &phrase,
      };
    }
    if (out_of_hours == nullptr) out_of_hours = &phrase;
  }

  if (out_of_hours != nullptr) {
    return Reading{
        .kind = Reading::Kind::kOutOfHours,
        .phrase = out_of_hours,
    };
  }

  return Reading{};
}

}  // namespace gm
