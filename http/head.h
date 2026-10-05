// The part of an HTTP/1.1 message that comes before the body: a start line
// and headers. Requests, responses and the WebSocket handshake all begin with
// one, and this is the only place that knows how it is written.

#ifndef HTTP_HEAD_H_
#define HTTP_HEAD_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "net/reader.h"
#include "net/stream.h"

namespace http {

struct Header {
  std::string name;
  std::string value;
};

// The value of the first header called `name`, compared without regard to
// case as HTTP requires, or an empty string if there is none.
std::string_view FindHeader(std::span<const Header> headers,
                            std::string_view name);

struct Head {
  // "GET /path HTTP/1.1" in a request, "HTTP/1.1 200 OK" in a response.
  std::string start_line;
  std::vector<Header> headers;
};

// The bytes that put `head` on the wire, up to and including the blank line
// that ends it.
std::string FormatHead(const Head& head);

// Reads a head from `reader`, leaving it positioned at the body. Fails with
// InvalidArgument if what arrives is not one, and as the reader does if the
// peer goes away or the deadline passes.
Task<absl::StatusOr<Head>> ReadHead(net::Reader& reader,
                                    net::Deadline deadline);

}  // namespace http

#endif  // HTTP_HEAD_H_
