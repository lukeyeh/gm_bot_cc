#include "gm/phrase.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
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

bool IsGm(std::string_view message, std::span<const std::string> phrases) {
  const std::string text = Folded(message);

  // The phrase has to stand as words of its own: "gmail" is not a GM.
  return std::ranges::any_of(phrases, [&text](const std::string& phrase) {
    std::string_view after = text;
    if (absl::ConsumePrefix(&after, phrase) &&
        (after.empty() || IsWordBreak(after.front()))) {
      return true;
    }

    std::string_view before = text;
    return absl::ConsumeSuffix(&before, phrase) &&
           (before.empty() || IsWordBreak(before.back()));
  });
}

}  // namespace gm
