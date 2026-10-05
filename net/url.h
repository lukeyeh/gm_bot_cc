// Splits a URL into the two things a client needs: where to connect, and what
// to ask for once connected.

#ifndef NET_URL_H_
#define NET_URL_H_

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "net/stream.h"

namespace net {

struct Url {
  // Where to dial. `https` and `wss` URLs are secured with TLS; `http` and
  // `ws` URLs are not. The port defaults to 443 or 80 accordingly.
  Address address;

  // The host, plus the port if the URL named one: the value of a Host header.
  std::string authority;

  // The path and query to request. Never empty: at least "/".
  std::string target;
};

// Parses an absolute http, https, ws or wss URL. Fails with kInvalidArgument
// for anything else.
absl::StatusOr<Url> ParseUrl(std::string_view url);

}  // namespace net

#endif  // NET_URL_H_
