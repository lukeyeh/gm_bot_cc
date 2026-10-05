#include "http/head.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "net/reader.h"
#include "net/stream.h"

namespace http {
namespace {

// Longer than any line a well-behaved peer sends.
constexpr size_t kMaxLineBytes = size_t{16} * 1024;

}  // namespace

std::string_view FindHeader(std::span<const Header> headers,
                            std::string_view name) {
  for (const Header& header : headers) {
    if (absl::EqualsIgnoreCase(header.name, name)) return header.value;
  }
  return {};
}

std::string FormatHead(const Head& head) {
  std::string text = absl::StrCat(head.start_line, "\r\n");
  for (const Header& header : head.headers) {
    absl::StrAppend(&text, header.name, ": ", header.value, "\r\n");
  }
  absl::StrAppend(&text, "\r\n");
  return text;
}

Task<absl::StatusOr<Head>> ReadHead(net::Reader& reader,
                                    net::Deadline deadline) {
  Head head;
  CO_ASSIGN_OR_RETURN(
      const std::string_view start_line,
      co_await reader.ReadUntil("\r\n", kMaxLineBytes, deadline));
  head.start_line = std::string(start_line);

  for (;;) {
    CO_ASSIGN_OR_RETURN(
        const std::string_view line,
        co_await reader.ReadUntil("\r\n", kMaxLineBytes, deadline));
    if (line.empty()) co_return head;

    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      co_return absl::InvalidArgumentError(
          absl::StrCat("malformed HTTP header: ", line));
    }

    head.headers.push_back(Header{
        .name = std::string(line.substr(0, colon)),
        .value =
            std::string(absl::StripAsciiWhitespace(line.substr(colon + 1))),
    });
  }
}

}  // namespace http
