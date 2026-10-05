// An HTTP client: give it a request, get back the server's response.
//
// The client owns everything between those two: connecting (with TLS for
// https URLs), keeping the connection open for the next request, reconnecting
// when the server has dropped it, and bounding how long any of it takes.
//
// Send is asynchronous (see async/task.h) and must be called from a task
// running on an EventLoop.

#ifndef HTTP_CLIENT_H_
#define HTTP_CLIENT_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "http/head.h"

namespace http {

enum class Method { kGet, kPost, kPut, kPatch, kDelete };

struct Request {
  Method method = Method::kGet;
  // Absolute, http or https.
  std::string url;
  // In addition to Host and Content-Length, which the client supplies.
  std::vector<Header> headers;
  std::string body;
};

struct Response {
  int status = 0;
  std::vector<Header> headers;
  std::string body;

  // The value of header `name`, or an empty string if it was not sent.
  std::string_view Get(std::string_view name) const {
    return FindHeader(headers, name);
  }
};

// A client makes one request at a time: wait for a Send to finish before
// starting the next.
class Client {
 public:
  virtual ~Client() = default;

  // Sends `request` and evaluates to the response, whatever its status: a 404
  // or a 500 is a response, not a failure. Fails only when no response could
  // be had: InvalidArgument for a URL that cannot be requested or a reply
  // that is not HTTP, Unavailable or NotFound if the server cannot be
  // reached, Unauthenticated if it is not who the URL says, DeadlineExceeded
  // if it is too slow.
  virtual Task<absl::StatusOr<Response>> Send(const Request& request) = 0;
};

// A client that talks to real servers.
std::unique_ptr<Client> NewClient();

}  // namespace http

#endif  // HTTP_CLIENT_H_
