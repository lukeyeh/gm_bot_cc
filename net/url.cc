#include "net/url.h"

#include <cstddef>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "net/stream.h"

namespace net {

absl::StatusOr<Url> ParseUrl(const std::string_view url) {
  const auto invalid = [url](const std::string_view why) {
    return absl::InvalidArgumentError(
        absl::StrCat("bad URL \"", url, "\": ", why));
  };

  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    return invalid("no scheme");
  }

  const std::string_view scheme = url.substr(0, scheme_end);
  Url parsed;
  if (scheme == "https" || scheme == "wss") {
    parsed.address.security = Security::kTls;
    parsed.address.port = 443;
  } else if (scheme == "http" || scheme == "ws") {
    parsed.address.security = Security::kPlaintext;
    parsed.address.port = 80;
  } else {
    return invalid("unsupported scheme");
  }

  const std::string_view rest = url.substr(scheme_end + 3);
  const size_t authority_end = rest.find_first_of("/?#");
  const std::string_view authority = rest.substr(0, authority_end);
  const size_t colon = authority.find(':');

  const std::string_view host = authority.substr(0, colon);
  if (host.empty()) {
    return invalid("no host");
  }
  if (colon != std::string_view::npos &&
      (!absl::SimpleAtoi(authority.substr(colon + 1), &parsed.address.port) ||
       parsed.address.port == 0)) {
    return invalid("bad port");
  }

  parsed.address.host = std::string(host);
  parsed.authority = std::string(authority);

  // The fragment is for the client alone and is never sent.
  const std::string_view target =
      authority_end == std::string_view::npos
          ? std::string_view()
          : rest.substr(authority_end, rest.find('#') - authority_end);
  parsed.target =
      target.starts_with('/') ? std::string(target) : absl::StrCat("/", target);

  return parsed;
}

}  // namespace net
